// Inventatory - Hardware Inventory Management System
// Matching KiCad BOM lines against inventory by value and package.

#include "core/BomMatch.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace inventatory {

using namespace std;

bool bomBuildReady(const BomAnalysis& analysis) {
  return analysis.shortCount == 0 && !analysis.matches.empty();
}

namespace {

// Imperial chip codes KiCad embeds in passive footprint names.
const initializer_list<const char*> kChipCodes = {"01005", "0201", "0402", "0603", "0805", "1206",
                                                  "1210", "1806", "1812", "2010", "2512"};

// Package families whose leading token already names the package.
const initializer_list<const char*> kPackageFamilies = {
    "HTSSOP", "TSSOP", "TSOT", "SSOP", "MSOP", "SOIC", "SOT", "SOD", "SOP", "QFN", "DFN",
    "TQFP",   "LQFP",  "QFP",  "BGA",  "WSON", "VSON", "SC",  "TO",  "DIP", "PDIP"};

vector<string> splitOnUnderscore(const string& value) {
  vector<string> tokens;
  string current;
  for (const char ch : value) {
    if (ch == '_') {
      if (!current.empty()) {
        tokens.push_back(current);
      }
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

bool startsWithInsensitive(const string& value, const string& prefix) {
  return value.size() >= prefix.size() && toLower(value.substr(0, prefix.size())) == toLower(prefix);
}

// Pulls "5.0" out of a "D5.0mm" diameter token.
string diameterToken(const vector<string>& tokens) {
  for (const auto& token : tokens) {
    if (token.size() > 2 && (token[0] == 'D' || token[0] == 'd') &&
        toLower(token).substr(token.size() - 2) == "mm") {
      return token.substr(1, token.size() - 3);  // drop the leading D and trailing mm
    }
  }
  return {};
}

// Pulls the pin count out of a "1x03" grid token.
string pinCountToken(const vector<string>& tokens) {
  for (const auto& token : tokens) {
    const auto lowered = toLower(token);
    const auto cross = lowered.find('x');
    if (cross == string::npos || cross == 0 || cross + 1 >= lowered.size()) {
      continue;
    }
    const auto rows = lowered.substr(0, cross);
    const auto columns = lowered.substr(cross + 1);
    if (!all_of(rows.begin(), rows.end(), [](unsigned char ch) { return isdigit(ch) != 0; }) ||
        !all_of(columns.begin(), columns.end(), [](unsigned char ch) { return isdigit(ch) != 0; })) {
      continue;
    }
    try {
      return to_string(stoi(rows) * stoi(columns));
    } catch (...) {
      return {};
    }
  }
  return {};
}

// Splits a multiplier letter off the numeric body, honouring RKM notation where
// the letter also stands in for the decimal point ("4R7", "1u5").
optional<double> parseNumberWithMultiplier(const string& body, bool resistanceLike) {
  if (body.empty()) {
    return nullopt;
  }

  size_t alpha = string::npos;
  for (size_t index = 0; index < body.size(); ++index) {
    if (isalpha(static_cast<unsigned char>(body[index])) != 0) {
      if (alpha != string::npos) {
        return nullopt;  // more than one letter is not a value
      }
      alpha = index;
    } else if (isdigit(static_cast<unsigned char>(body[index])) == 0 && body[index] != '.') {
      return nullopt;
    }
  }

  string numberText = body;
  double multiplier = 1.0;

  if (alpha != string::npos) {
    const auto letter = static_cast<char>(tolower(static_cast<unsigned char>(body[alpha])));
    const auto head = body.substr(0, alpha);
    const auto tail = body.substr(alpha + 1);

    switch (letter) {
      case 'p':
        multiplier = 1e-12;
        break;
      case 'n':
        multiplier = 1e-9;
        break;
      case 'u':
        multiplier = 1e-6;
        break;
      case 'm':
        // For resistors a bare M means megaohm; for reactive units it is milli.
        multiplier = resistanceLike ? 1e6 : 1e-3;
        break;
      case 'k':
        multiplier = 1e3;
        break;
      case 'g':
        multiplier = 1e9;
        break;
      case 'r':
      case 'e':
        multiplier = 1.0;
        break;
      default:
        return nullopt;
    }
    // An uppercase M is always mega, whatever the unit.
    if (body[alpha] == 'M') {
      multiplier = 1e6;
    }

    if (!tail.empty()) {
      numberText = (head.empty() ? string("0") : head) + "." + tail;
    } else {
      numberText = head;
    }
  }

  if (numberText.empty()) {
    return nullopt;
  }

  try {
    size_t consumed = 0;
    const double number = stod(numberText, &consumed);
    if (consumed != numberText.size()) {
      return nullopt;
    }
    return number * multiplier;
  } catch (...) {
    return nullopt;
  }
}

bool endsWith(const string& value, const string& suffix) {
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

string compactKey(const string& value) {
  string compact;
  compact.reserve(value.size());
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      compact.push_back(static_cast<char>(tolower(ch)));
    }
  }
  return compact;
}

// The item-side value for a kind, taken from DigiKey parameters when present
// and otherwise recovered from the description text.
optional<double> itemValueFor(const InventoryItem& item, ValueKind kind) {
  // The initializer lists stay inline: binding one to a named variable would
  // leave the backing array dangling past the full expression.
  optional<string> text;
  switch (kind) {
    case ValueKind::Capacitance:
      text = parameterValue(item, {"Capacitance", "Value"});
      break;
    case ValueKind::Resistance:
      text = parameterValue(item, {"Resistance", "Value"});
      break;
    case ValueKind::Inductance:
      text = parameterValue(item, {"Inductance", "Value"});
      break;
    case ValueKind::Frequency:
      text = parameterValue(item, {"Frequency", "Value"});
      break;
    case ValueKind::None:
      return nullopt;
  }

  if (text) {
    ValueKind parsedKind = ValueKind::None;
    if (const auto value = parseElectricalValue(*text, parsedKind); value && parsedKind == kind) {
      return value;
    }
  }

  // DigiKey descriptions such as "CAP CER 1UF 25V X7R 0603" carry the value in
  // free text, which covers items that were never enriched.
  for (const auto& token : tokenizeQuery(item.partName + " " + item.notes)) {
    ValueKind parsedKind = ValueKind::None;
    if (const auto value = parseElectricalValue(token, parsedKind); value && parsedKind == kind) {
      return value;
    }
  }
  return nullopt;
}

optional<string> itemPackage(const InventoryItem& item) {
  if (const auto package = parameterValue(item, {"Package / Case", "Package Case", "Case / Package",
                                                 "Case Package", "Supplier Device Package", "Device Package",
                                                 "Package"})) {
    return package;
  }
  // Fall back to a chip code sitting in the description.
  const auto compact = compactKey(item.partName);
  for (const auto* code : kChipCodes) {
    if (compact.find(code) != string::npos) {
      return string(code);
    }
  }
  return nullopt;
}

bool sameText(const string& lhs, const string& rhs) {
  return !trim(lhs).empty() && toLower(trim(lhs)) == toLower(trim(rhs));
}

bool partNameHasToken(const InventoryItem& item, const string& designation) {
  const auto needle = compactKey(designation);
  if (needle.size() < 4) {
    return false;
  }
  return compactKey(item.partName + " " + item.notes + " " + item.sku).find(needle) != string::npos;
}

int scoreItem(const InventoryItem& item, const BomLine& line, const string& bomPackage,
              optional<double> bomValue, ValueKind bomKind) {
  if (sameText(line.designation, item.sku) || sameText(line.designation, item.digikeyPartNumber)) {
    return 100;
  }
  if (looksLikePartNumber(line.designation) && partNameHasToken(item, line.designation)) {
    return 85;
  }

  if (!bomValue) {
    return 0;
  }

  const auto candidateValue = itemValueFor(item, bomKind);
  if (!candidateValue) {
    return 0;
  }
  const double reference = max(fabs(*bomValue), 1e-18);
  if (fabs(*candidateValue - *bomValue) / reference > 0.01) {
    return 0;
  }

  const auto package = itemPackage(item);
  if (bomPackage.empty() || !package) {
    return 70;
  }
  return packageMatches(bomPackage, *package) ? 90 : 30;
}

}  // namespace

string packageFromFootprint(const string& footprint) {
  const auto trimmed = trim(footprint);
  if (trimmed.empty()) {
    return {};
  }

  const auto tokens = splitOnUnderscore(trimmed);

  // Chip passives: the imperial code is a standalone token.
  for (const auto& token : tokens) {
    for (const auto* code : kChipCodes) {
      if (token == code) {
        return code;
      }
    }
  }

  // Through-hole electrolytics carry their body diameter instead of a code.
  if (compactKey(trimmed).find("radial") != string::npos) {
    const auto diameter = diameterToken(tokens);
    return diameter.empty() ? string("Radial") : "Radial " + diameter + "mm";
  }

  // Connectors are identified by series plus pin count.
  if (!tokens.empty() && toLower(tokens.front()) == "jst") {
    const auto pins = pinCountToken(tokens);
    const string series = tokens.size() > 1 ? tokens[1] : string();
    string label = "JST " + series;
    if (!pins.empty()) {
      label += " " + pins;
    }
    return trim(label);
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

optional<double> parseElectricalValue(const string& text, ValueKind& kind) {
  kind = ValueKind::None;

  string compact;
  for (const char ch : trim(text)) {
    if (!isspace(static_cast<unsigned char>(ch))) {
      compact.push_back(ch);
    }
  }
  if (compact.empty()) {
    return nullopt;
  }

  const auto lowered = toLower(compact);
  ValueKind detected = ValueKind::Resistance;
  string body = compact;

  if (endsWith(lowered, "hz")) {
    detected = ValueKind::Frequency;
    body = compact.substr(0, compact.size() - 2);
  } else if (endsWith(lowered, "ohms")) {
    body = compact.substr(0, compact.size() - 4);
  } else if (endsWith(lowered, "ohm")) {
    body = compact.substr(0, compact.size() - 3);
  } else if (endsWith(lowered, "f")) {
    detected = ValueKind::Capacitance;
    body = compact.substr(0, compact.size() - 1);
  } else if (endsWith(lowered, "h")) {
    detected = ValueKind::Inductance;
    body = compact.substr(0, compact.size() - 1);
  }

  // A bare number with no unit is ambiguous; only a resistor is ever written
  // that way, and even then it needs at least one digit.
  if (body.empty() || none_of(body.begin(), body.end(), [](unsigned char ch) { return isdigit(ch) != 0; })) {
    return nullopt;
  }

  const auto value = parseNumberWithMultiplier(body, detected == ValueKind::Resistance);
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
  return compactKey(line.designation) + "|" + compactKey(line.footprint);
}

void recomputeBomTotals(BomAnalysis& analysis, const vector<InventoryItem>& items) {
  unordered_map<string, const InventoryItem*> byId;
  byId.reserve(items.size());
  for (const auto& item : items) {
    byId.emplace(item.id, &item);
  }

  // One inventory part can serve several BOM lines, so sufficiency has to be
  // judged against the total draw on that part, not line by line.
  unordered_map<string, int> demand;
  for (size_t index = 0; index < analysis.matches.size(); ++index) {
    auto& match = analysis.matches[index];
    match.needed = analysis.lines[index].quantityPerBoard * max(1, analysis.boards);
    const auto itemId = match.chosenItemId();
    if (!itemId.empty()) {
      demand[itemId] += match.needed;
    }
  }

  analysis.readyCount = 0;
  analysis.shortCount = 0;
  analysis.totalPieces = 0;
  for (auto& match : analysis.matches) {
    analysis.totalPieces += match.needed;
    const auto itemId = match.chosenItemId();
    const auto found = itemId.empty() ? byId.end() : byId.find(itemId);
    match.available = found == byId.end() ? 0 : found->second->quantity;
    match.sufficient = found != byId.end() && found->second->quantity >= demand[itemId];
    if (match.sufficient) {
      ++analysis.readyCount;
    } else {
      ++analysis.shortCount;
    }
  }
}

BomAnalysis analyzeBom(const KicadBomFile& bom, const vector<InventoryItem>& items, int boards,
                       const map<string, string>& overrides) {
  BomAnalysis analysis;
  analysis.boards = max(1, boards);
  analysis.lines = bom.lines;
  analysis.matches.reserve(bom.lines.size());

  for (size_t index = 0; index < bom.lines.size(); ++index) {
    const auto& line = bom.lines[index];
    BomMatch match;
    match.lineIndex = index;

    const auto bomPackage = packageFromFootprint(line.footprint);
    ValueKind bomKind = ValueKind::None;
    const auto bomValue = parseElectricalValue(line.designation, bomKind);

    for (const auto& item : items) {
      const auto score = scoreItem(item, line, bomPackage, bomValue, bomKind);
      if (score >= kBomMatchThreshold) {
        match.candidates.push_back({item.id, score});
      }
    }

    // Best score first; a bigger reel breaks ties so the pick is stable and the
    // user is steered away from a nearly empty slot.
    stable_sort(match.candidates.begin(), match.candidates.end(),
                [&](const BomMatchCandidate& lhs, const BomMatchCandidate& rhs) {
                  if (lhs.score != rhs.score) {
                    return lhs.score > rhs.score;
                  }
                  const auto find = [&](const string& id) {
                    const auto it = find_if(items.begin(), items.end(),
                                            [&](const InventoryItem& item) { return item.id == id; });
                    return it == items.end() ? 0 : it->quantity;
                  };
                  return find(lhs.itemId) > find(rhs.itemId);
                });

    const auto pinnedOverride = overrides.find(bomLineKey(line));
    if (pinnedOverride != overrides.end()) {
      const auto pinned = find_if(match.candidates.begin(), match.candidates.end(),
                                  [&](const BomMatchCandidate& candidate) {
                                    return candidate.itemId == pinnedOverride->second;
                                  });
      if (pinned != match.candidates.end()) {
        match.chosen = static_cast<size_t>(distance(match.candidates.begin(), pinned));
      }
    }

    analysis.matches.push_back(move(match));
  }

  recomputeBomTotals(analysis, items);
  return analysis;
}

}  // namespace inventatory
