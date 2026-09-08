// Inventatory - Hardware Inventory Management System
// Matching KiCad BOM lines against inventory by value and package.

#include "core/BomMatch.h"

#include "BomMatchPrivate.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace inventatory {

using namespace std;
using namespace bom_match_detail;

bool bomBuildReady(const BomAnalysis& analysis) {
  return analysis.shortCount == 0 && !analysis.matches.empty();
}

string packageFromFootprint(const string& footprint) {
  const auto trimmed = trim(footprint);
  if (trimmed.empty()) {
    return {};
  }

  const auto tokens = splitOnUnderscore(trimmed);

  // Chip passives: the imperial code is a standalone token.
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
  // Keep aggregate demand wider than the public per-line `int` fields. A
  // saturating int sum would make two INT_MAX BOM lines look satisfiable by
  // INT_MAX stock even though their combined draw is larger.
  unordered_map<string, uint64_t> demand;
  for (size_t index = 0; index < analysis.matches.size(); ++index) {
    auto& match = analysis.matches[index];
    match.needed = saturatingMultiplyNonNegative(analysis.lines[index].quantityPerBoard, max(1, analysis.boards));
    const auto itemId = match.chosenItemId();
    if (!itemId.empty()) {
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
  analysis.totalPieces = 0;
  for (auto& match : analysis.matches) {
    analysis.totalPieces = saturatingAdd(analysis.totalPieces, match.needed);
    const auto itemId = match.chosenItemId();
    const auto found = itemId.empty() ? byId.end() : byId.find(itemId);
    match.available = found == byId.end() ? 0 : found->second->quantity;
    match.sufficient = found != byId.end() && found->second->quantity >= 0 &&
                       static_cast<uint64_t>(found->second->quantity) >= demand[itemId];
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
