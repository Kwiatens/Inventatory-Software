// Inventatory - Hardware Inventory Management System
// Matching KiCad BOM lines against inventory by value and package.

#pragma once

#include "core/Inventory.h"
#include "import/KicadBom.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

using std::map;
using std::optional;
using std::string;
using std::vector;

enum class ValueKind {
  None,
  Capacitance,
  Resistance,
  Inductance,
  Frequency,
};

// Reduces a KiCad footprint to the package token used by distributor data:
// "C_0603_1608Metric" -> "0603", "SOIC-8_3.9x4.9mm_P1.27mm" -> "SOIC-8".
string packageFromFootprint(const string& footprint);

// True when two package descriptions refer to the same physical package.
bool packageMatches(const string& lhs, const string& rhs);

// Parses "1uF", "100nF", "10K", "280R", "R280" (RKM), "4.7uH" and "32.768KHz"
// into SI base units. Leading-R notation follows the RKM standard, so "R280"
// is 0.28 ohm while "280R" is 280 ohm.
optional<double> parseElectricalValue(const string& text, ValueKind& kind);

// True when a designation reads as a part number rather than a component value.
bool looksLikePartNumber(const string& designation);

// Stable key identifying a BOM line across re-analysis, used for overrides.
string bomLineKey(const BomLine& line);

struct BomMatchCandidate {
  string itemId;
  int score = 0;
};

struct BomMatch {
  size_t lineIndex = 0;
  vector<BomMatchCandidate> candidates;  // ranked, best first; empty means no stock
  size_t chosen = 0;
  int needed = 0;     // quantityPerBoard * boards
  int available = 0;  // chosen item's on-hand quantity
  bool sufficient = false;

  const BomMatchCandidate* chosenCandidate() const {
    return chosen < candidates.size() ? &candidates[chosen] : nullptr;
  }
  string chosenItemId() const {
    const auto* candidate = chosenCandidate();
    return candidate == nullptr ? string() : candidate->itemId;
  }
};

struct BomAnalysis {
  int boards = 1;
  vector<BomLine> lines;
  vector<BomMatch> matches;  // parallel to lines
  int readyCount = 0;
  int shortCount = 0;
  int totalPieces = 0;
};

// A build may be browsed while short, but it can only be completed when every
// BOM line has a selected stock item with enough units for all requested boards.
bool bomBuildReady(const BomAnalysis& analysis);

// Score at or above which a candidate is trusted enough to auto-select.
constexpr int kBomMatchThreshold = 60;

BomAnalysis analyzeBom(const KicadBomFile& bom, const vector<InventoryItem>& items, int boards,
                       const map<string, string>& overrides);

// Recomputes needed/available/sufficient after a manual override or a board
// count change, without rescoring every candidate.
void recomputeBomTotals(BomAnalysis& analysis, const vector<InventoryItem>& items);

}  // namespace inventatory
