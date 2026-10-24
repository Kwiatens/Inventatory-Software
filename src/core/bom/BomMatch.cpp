// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#include "core/bom/BomMatch.h"

#include "BomMatchPrivate.h"

#include "core/inventory/ParameterLookup.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <regex>
#include <unordered_map>

namespace inventatory {

using namespace std;
using namespace bom_match_detail;

namespace {

// Metric chip sizes and the imperial codes they name. None collides with kChipCodes,
// so "0603" is never read as metric.
struct MetricChipCode {
  const char* metric;
  const char* imperial;
};
constexpr MetricChipCode kMetricChipCodes[] = {
    {"1005", "0402"}, {"1608", "0603"}, {"2012", "0805"}, {"3216", "1206"},
    {"3225", "1210"}, {"4532", "1812"}, {"5025", "2010"}, {"6332", "2512"},
};

// An imperial code written alone or with one or two letters in front: "0402", "C0402", "R0603".
string imperialCodeFromToken(const string& token) {
  size_t start = 0;
  while (start < token.size() && start < 2 && isalpha(static_cast<unsigned char>(token[start])) != 0) ++start;
  const auto digits = token.substr(start);
  for (const auto* code : kChipCodes) {
    if (digits == code) return code;
  }
  return {};
}

// IPC-7351 chip names carry the metric size between the letters and an X: "CAPC1608X90N" is 0603.
string imperialCodeFromIpcToken(const string& token) {
  size_t letters = 0;
  while (letters < token.size() && isalpha(static_cast<unsigned char>(token[letters])) != 0) ++letters;
  if (letters < 3 || letters > 6 || token.size() < letters + 5) return {};
  const auto metric = token.substr(letters, 4);
  if (!all_of(metric.begin(), metric.end(), [](unsigned char ch) { return isdigit(ch) != 0; })) return {};
  const auto after = token[letters + 4];
  if (after != 'X' && after != 'x') return {};
  for (const auto& alias : kMetricChipCodes) {
    if (metric == alias.metric) return alias.imperial;
  }
  return {};
}

// The imperial chip code a package names: "0603", "0603 (1608 Metric)". Empty otherwise.
string chipCodeOfPackage(const string& package) {
  const auto trimmed = trim(package);
  const auto end = trimmed.find_first_of(" (");
  return imperialCodeFromToken(trimmed.substr(0, end));
}

}  // namespace

bool bomBuildReady(const BomAnalysis& analysis) {
  return analysis.shortCount == 0 && analysis.confirmCount == 0 && !analysis.matches.empty();
}

string footprintName(const string& footprint) {
  const auto trimmed = trim(footprint);
  const auto colon = trimmed.rfind(':');
  return colon == string::npos ? trimmed : trim(trimmed.substr(colon + 1));
}

string packageFromFootprint(const string& footprint) {
  const auto trimmed = footprintName(footprint);
  if (trimmed.empty()) {
    return {};
  }

  const auto tokens = splitOnUnderscore(trimmed);

  for (const auto& token : tokens) {
    for (const auto* code : kChipCodes) {
      if (toLower(token) == toLower(code)) {
        return code;
      }
    }
  }

  // Through-hole electrolytics carry their body diameter instead of a code.
  if (compactKey(trimmed).find("radial") != string::npos) {
    const auto diameter = diameterToken(tokens);
    return diameter.empty() ? string("Radial") : "Radial " + diameter + "mm";
  }

  if (!tokens.empty() && toLower(tokens.front()) == "jst") {
    const auto pins = pinCountToken(tokens);
    const string series = tokens.size() > 1 ? tokens[1] : string();
    string label = "JST " + series;
    if (!pins.empty()) {
      label += " " + pins;
    }
    return trim(label);
  }

  // EasyEDA ("R0402") and IPC-7351 ("CAPC1608X90N") names carry the same chip size as KiCad codes.
  if (!tokens.empty()) {
    if (const auto code = imperialCodeFromToken(tokens.front()); !code.empty()) return code;
    if (const auto code = imperialCodeFromIpcToken(tokens.front()); !code.empty()) return code;
  }

  const auto head = tokens.empty() ? trimmed : tokens.front();
  for (const auto* family : kPackageFamilies) {
    if (startsWithInsensitive(head, family)) {
      return head;
    }
  }

  return head;
}

bool packageMatches(const string& lhs, const string& rhs) {
  // Chip codes must match exactly: "0402" must not match a package that contains those digits.
  const auto leftCode = chipCodeOfPackage(lhs);
  const auto rightCode = chipCodeOfPackage(rhs);
  if (!leftCode.empty() && !rightCode.empty()) return leftCode == rightCode;

  const auto left = compactKey(lhs);
  const auto right = compactKey(rhs);
  if (left.empty() || right.empty()) {
    return false;
  }
  if (left == right) {
    return true;
  }
  // Short tokens such as "TO" would match far too much as substrings.
  if (left.size() < 3 || right.size() < 3) {
    return false;
  }
  return left.find(right) != string::npos || right.find(left) != string::npos;
}

namespace {

// A comma is the decimal point in a value written the European way ("0,33uF", "4,7k", ",33"). It groups
// thousands only in the form "1,000", "10,000" or "4,700,000", where the first group has one to three digits
// and no zero. A multiplier or unit may follow the number: "1,000pF" is 1000 pF. Any other comma is invalid.
optional<string> readDecimalComma(const string& body) {
  if (body.find(',') == string::npos) return body;
  static const regex thousands(R"(^([1-9]\d{0,2})((?:,\d{3})+)([^\d,].*)?$)");
  static const regex decimal(R"(^(\d*),(\d+)([^\d,].*)?$)");
  smatch m;
  if (regex_match(body, m, thousands)) {
    string digits = m[1];
    for (const char ch : string(m[2])) {
      if (ch != ',') digits.push_back(ch);
    }
    return digits + string(m[3]);
  }
  if (!regex_match(body, m, decimal)) return nullopt;
  return (m[1].length() == 0 ? string("0") : string(m[1])) + "." + string(m[2]) + string(m[3]);
}

}  // namespace

optional<double> parseElectricalValue(const string& text, ValueKind& kind) {
  kind = ValueKind::None;

  string compact;
  for (const char ch : trim(text)) {
    if (!isspace(static_cast<unsigned char>(ch))) {
      compact.push_back(ch);
    }
  }
  // Map the UTF-8 micro and ohm signs to the ASCII units "u" and "ohm".
  for (const auto& [sign, ascii] : {pair<const char*, const char*>{"\xC2\xB5", "u"},
                                    {"\xCE\xBC", "u"},
                                    {"\xCE\xA9", "ohm"},
                                    {"\xE2\x84\xA6", "ohm"}}) {
    for (size_t at = compact.find(sign); at != string::npos; at = compact.find(sign, at)) {
      const auto length = string(sign).size();
      compact.replace(at, length, ascii);
    }
  }
  if (compact.empty()) {
    return nullopt;
  }

  const auto lowered = toLower(compact);
  ValueKind detected = ValueKind::Resistance;
  NumberNotation notation = NumberNotation::Unitless;
  string body = compact;

  if (endsWith(lowered, "hz")) {
    detected = ValueKind::Frequency;
    notation = NumberNotation::Reactive;
    body = compact.substr(0, compact.size() - 2);
  } else if (endsWith(lowered, "ohms")) {
    notation = NumberNotation::Resistance;
    body = compact.substr(0, compact.size() - 4);
  } else if (endsWith(lowered, "ohm")) {
    notation = NumberNotation::Resistance;
    body = compact.substr(0, compact.size() - 3);
  } else if (endsWith(lowered, "f")) {
    detected = ValueKind::Capacitance;
    notation = NumberNotation::Reactive;
    body = compact.substr(0, compact.size() - 1);
  } else if (endsWith(lowered, "h")) {
    detected = ValueKind::Inductance;
    notation = NumberNotation::Reactive;
    body = compact.substr(0, compact.size() - 1);
  }

  const auto number = readDecimalComma(body);
  if (!number) return nullopt;
  body = *number;

  // A bare number is ambiguous; only a resistor is written that way, and it needs a digit.
  if (body.empty() || none_of(body.begin(), body.end(), [](unsigned char ch) { return isdigit(ch) != 0; })) {
    return nullopt;
  }

  // Bare chip codes ("0603") name a package, not an ohm value.
  if (notation == NumberNotation::Unitless) {
    for (const auto* code : kChipCodes) {
      if (body == code) {
        return nullopt;
      }
    }
  }

  const auto value = parseNumberWithMultiplier(body, notation);
  if (!value) {
    return nullopt;
  }

  kind = detected;
  return value;
}

bool looksLikePartNumber(const string& designation) {
  const auto trimmed = trim(designation);
  if (trimmed.size() < 4) {
    return false;
  }
  ValueKind kind = ValueKind::None;
  if (parseElectricalValue(trimmed, kind)) {
    return false;
  }
  const bool hasDigit = any_of(trimmed.begin(), trimmed.end(),
                               [](unsigned char ch) { return isdigit(ch) != 0; });
  const bool hasAlpha = any_of(trimmed.begin(), trimmed.end(),
                               [](unsigned char ch) { return isalpha(ch) != 0; });
  return hasDigit && hasAlpha;
}

string bomLineKey(const BomLine& line) {
  // Keys without supplier or MPN match those from before identity columns. Other part numbers
  // get their own key, so a confirmation never applies to the wrong row.
  string key = compactKey(line.designation) + "|" + compactKey(line.footprint);
  if (!line.supplierPartNumber.empty()) {
    key += "|" + compactKey(line.supplier) + ":" + compactKey(line.supplierPartNumber);
  }
  if (!line.manufacturerPartNumber.empty()) {
    key += "|mpn:" + compactKey(line.manufacturerPartNumber);
  }
  return key;
}

void recomputeBomTotals(BomAnalysis& analysis, const vector<InventoryItem>& items) {
  unordered_map<string, const InventoryItem*> byId;
  byId.reserve(items.size());
  for (const auto& item : items) {
    byId.emplace(item.id, &item);
  }

  // One part can serve several lines, so judge sufficiency on the total draw of confirmed lines.
  // Sum in 64 bits: a saturating int sum would let two INT_MAX lines pass on INT_MAX stock.
  unordered_map<string, uint64_t> demand;
  for (size_t index = 0; index < analysis.matches.size(); ++index) {
    auto& match = analysis.matches[index];
    match.needed = saturatingMultiplyNonNegative(analysis.lines[index].quantityPerBoard, max(1, analysis.boards));
    const auto itemId = match.chosenItemId();
    if (!itemId.empty() && match.confirmed) {
      auto& total = demand[itemId];
      const auto needed = static_cast<uint64_t>(match.needed);
      if (total > numeric_limits<uint64_t>::max() - needed) {
        total = numeric_limits<uint64_t>::max();
      } else {
        total += needed;
      }
    }
  }

  analysis.readyCount = 0;
  analysis.shortCount = 0;
  analysis.confirmCount = 0;
  analysis.totalPieces = 0;
  for (auto& match : analysis.matches) {
    analysis.totalPieces = saturatingAdd(analysis.totalPieces, match.needed);
    const auto itemId = match.chosenItemId();
    const auto found = itemId.empty() ? byId.end() : byId.find(itemId);
    match.available = found == byId.end() ? 0 : found->second->quantity;
    match.sufficient = match.confirmed && found != byId.end() && found->second->quantity >= 0 &&
                       static_cast<uint64_t>(found->second->quantity) >= demand[itemId];
    if (match.sufficient) {
      ++analysis.readyCount;
    } else if (match.needsConfirmation()) {
      ++analysis.confirmCount;
    } else {
      ++analysis.shortCount;
    }
  }
}

namespace {

// Identity lines carry a part number. They are accepted only on an identifier match, not by value.
bool carriesPartIdentifier(const BomLine& line) {
  return !line.manufacturerPartNumber.empty() || !line.supplierPartNumber.empty() ||
         looksLikePartNumber(line.designation);
}

// Why a line with candidates needs the user. The same rules apply to every BOM dialect.
BomCheckReason automaticReason(const BomMatch& match, const BomLine& line) {
  if (match.candidates.empty()) return BomCheckReason::None;  // nothing to choose from: a shortage
  if (line.quantityConflict) return BomCheckReason::QuantityConflict;
  if (!carriesPartIdentifier(line)) {
    if (match.candidates.size() != 1) return BomCheckReason::Ambiguous;
    // Accepted only when value and package both match (score 90).
    // Score 70 (package unknown) needs confirming.
    return match.candidates.front().score >= 90 ? BomCheckReason::None : BomCheckReason::Unverified;
  }
  const auto identityCount = count_if(match.candidates.begin(), match.candidates.end(),
                                      [](const BomMatchCandidate& candidate) { return candidate.identity; });
  if (identityCount == 0) return BomCheckReason::IdentityNotInStock;
  if (identityCount == 1 && match.candidates.front().identity) return BomCheckReason::None;
  return BomCheckReason::Ambiguous;
}

}  // namespace

string bomItemFingerprint(const InventoryItem& item) {
  // Only fields the matcher reads: quantity, price and location must not invalidate a confirmation.
  string text;
  const auto add = [&text](const string& value) {
    text += value;
    text.push_back('\x1f');
  };
  add(item.partName);
  add(item.notes);
  add(item.sku);
  add(item.digikeyPartNumber);
  add(item.vendorMetadata.provider);
  add(item.vendorMetadata.providerProductNumber);
  add(item.vendorMetadata.manufacturerPartNumber);
  for (const auto& parameter : item.parameters) {
    add(parameter.name);
    add(parameter.value);
  }
  // FNV-1a: unlike std::hash, the same in every build, so stored fingerprints stay valid.
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= 1099511628211ULL;
  }
  static const char kDigits[] = "0123456789abcdef";
  string hex(16, '0');
  for (size_t index = 0; index < hex.size(); ++index) {
    hex[hex.size() - 1 - index] = kDigits[(hash >> (4 * index)) & 0xF];
  }
  return hex;
}

string encodeBomConfirmation(const BomConfirmation& confirmation) {
  return confirmation.itemId + '\x1e' + confirmation.fingerprint;
}

optional<BomConfirmation> decodeBomConfirmation(const string& value) {
  const auto separator = value.find('\x1e');
  if (separator == string::npos || separator == 0 || separator + 1 >= value.size()) {
    return nullopt;
  }
  BomConfirmation confirmation{value.substr(0, separator), value.substr(separator + 1)};
  if (confirmation.fingerprint.size() != 16 ||
      !all_of(confirmation.fingerprint.begin(), confirmation.fingerprint.end(),
              [](unsigned char ch) { return isxdigit(ch) != 0; })) {
    return nullopt;
  }
  return confirmation;
}

bool bomSuggestionFitsLine(const BomLine& line, const InventoryItem& suggestion) {
  const auto bomPackage = packageFromFootprint(line.footprint);
  ValueKind bomKind = ValueKind::None;
  const auto bomValue = parseElectricalValue(line.designation, bomKind);
  const auto scored = scoreItem(suggestion, line, bomPackage, bomValue, bomKind, precomputeItemValues(suggestion));
  // A line naming a part is verified only by that part.
  // Otherwise value and package verify it, as for stock.
  if (carriesPartIdentifier(line)) return scored.identity;
  return scored.score >= 90;
}

BomAnalysis analyzeBom(const KicadBomFile& bom, const vector<InventoryItem>& items, int boards,
                       const map<string, string>& confirmations, const map<string, string>& suggestions) {
  BomAnalysis analysis;
  analysis.boards = max(1, boards);
  analysis.lines = bom.lines;
  analysis.matches.reserve(bom.lines.size());

  unordered_map<string, int> quantities;
  quantities.reserve(items.size());
  unordered_map<string, size_t> itemIndexById;
  itemIndexById.reserve(items.size());
  for (size_t index = 0; index < items.size(); ++index) {
    quantities.emplace(items[index].id, items[index].quantity);
    itemIndexById.emplace(items[index].id, index);
  }

  vector<ItemValueCache> itemCaches;
  itemCaches.reserve(items.size());
  for (const auto& item : items) {
    itemCaches.push_back(precomputeItemValues(item));
  }

  for (size_t index = 0; index < bom.lines.size(); ++index) {
    const auto& line = bom.lines[index];
    BomMatch match;
    match.lineIndex = index;

    const auto bomPackage = packageFromFootprint(line.footprint);
    ValueKind bomKind = ValueKind::None;
    const auto bomValue = parseElectricalValue(line.designation, bomKind);

    for (size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
      const auto& item = items[itemIndex];
      const auto scored = scoreItem(item, line, bomPackage, bomValue, bomKind, itemCaches[itemIndex]);
      if (scored.score >= kBomMatchThreshold) {
        match.candidates.push_back({item.id, scored.score, scored.identity});
      }
    }

    // Best score first, then larger stock. The order only picks the suggestion,
    // and never authorises a match.
    stable_sort(match.candidates.begin(), match.candidates.end(),
                [&](const BomMatchCandidate& lhs, const BomMatchCandidate& rhs) {
                  if (lhs.score != rhs.score) {
                    return lhs.score > rhs.score;
                  }
                  const auto find = [&](const string& id) {
                    const auto it = quantities.find(id);
                    return it == quantities.end() ? 0 : it->second;
                  };
                  return find(lhs.itemId) > find(rhs.itemId);
                });

    const auto key = bomLineKey(line);
    bool confirmed = false;
    size_t chosen = 0;
    BomCheckReason reason = automaticReason(match, line);

    // A saved confirmation holds only for the same item and fingerprint. Otherwise it is reported
    // lost, and the line falls back unconfirmed rather than being replaced silently.
    const auto saved = confirmations.find(key);
    if (saved != confirmations.end() && saved->second == kBomNotInStock) {
      match.candidates.clear();
      reason = BomCheckReason::None;
    } else if (saved != confirmations.end()) {
      const auto confirmation = decodeBomConfirmation(saved->second);
      // A part the user confirmed counts even when the matcher would not list it, such as a part received
      // for this line: the fingerprint still guards against a later change.
      if (confirmation) {
        const auto itemIndex = itemIndexById.find(confirmation->itemId);
        const bool listed = any_of(match.candidates.begin(), match.candidates.end(),
                                   [&](const BomMatchCandidate& entry) { return entry.itemId == confirmation->itemId; });
        if (!listed && itemIndex != itemIndexById.end() &&
            bomItemFingerprint(items[itemIndex->second]) == confirmation->fingerprint) {
          match.candidates.push_back({confirmation->itemId, 0, false});
        }
      }
      const auto itemIndex = confirmation ? itemIndexById.find(confirmation->itemId) : itemIndexById.end();
      const auto candidate = confirmation ? find_if(match.candidates.begin(), match.candidates.end(),
                                                    [&](const BomMatchCandidate& entry) {
                                                      return entry.itemId == confirmation->itemId;
                                                    })
                                          : match.candidates.end();
      if (confirmation && candidate != match.candidates.end() && itemIndex != itemIndexById.end() &&
          bomItemFingerprint(items[itemIndex->second]) == confirmation->fingerprint) {
        chosen = static_cast<size_t>(distance(match.candidates.begin(), candidate));
        confirmed = true;
        reason = BomCheckReason::None;
      } else {
        reason = BomCheckReason::OverrideLost;
      }
    } else if (reason == BomCheckReason::None && !match.candidates.empty()) {
      confirmed = true;
    }

    // "Next match" moves the suggestion but never confirms it.
    const auto suggested = suggestions.find(key);
    if (suggested != suggestions.end()) {
      const auto candidate = find_if(match.candidates.begin(), match.candidates.end(),
                                     [&](const BomMatchCandidate& entry) { return entry.itemId == suggested->second; });
      const auto position = static_cast<size_t>(distance(match.candidates.begin(), candidate));
      if (candidate != match.candidates.end() && position != chosen) {
        chosen = position;
        confirmed = false;
        if (reason == BomCheckReason::None) reason = BomCheckReason::Ambiguous;
      }
    }

    match.chosen = chosen;
    match.confirmed = confirmed && !match.candidates.empty();
    match.check = match.confirmed ? BomCheckReason::None : reason;
    analysis.matches.push_back(move(match));
  }

  recomputeBomTotals(analysis, items);
  return analysis;
}

}  // namespace inventatory
