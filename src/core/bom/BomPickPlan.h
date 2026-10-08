// Inventatory - Hardware Inventory Management System
// Find in racks: the rack-by-rack pick route for a BOM analysis, and the sentences that guide it.

#pragma once

#include "core/bom/BomMatch.h"
#include "core/inventory/Inventory.h"
#include "core/text/Voice.h"

#include <cstddef>
#include <string>
#include <vector>

namespace inventatory {

// One BOM line to take from stock.
struct BomPick {
  size_t matchIndex = 0;  // index into BomAnalysis::matches
  std::string itemId;
  std::string slot;         // rack slot such as "B3", or the item's location for a loose pick
  std::string designation;  // the BOM value as written ("100nF")
  std::string package;
  std::vector<std::string> designators;
  int needed = 0;    // what the BOM asks for
  int quantity = 0;  // what stock can supply now: min(needed, on hand)
};

// One place to visit: a rack, or the final stop for parts kept outside racks.
struct BomPickStop {
  std::string rackId;  // empty for the loose stop
  std::string title;   // "Rack 1" or "Outside racks"
  std::string componentType;
  std::vector<BomPick> picks;

  int pieces() const;
};

struct BomPickPlan {
  std::vector<BomPickStop> stops;
  // Matches that stock cannot fully cover. Lines with nothing on hand never appear as picks; lines
  // with some stock are picked as far as stock allows and are listed here as well.
  std::vector<size_t> shortMatches;
  int pieces = 0;      // everything the stops ask for
  int pickLines = 0;   // BOM lines with at least one piece to take

  bool empty() const { return stops.empty(); }
};

// Rack stops come first in rack order, each sorted by slot, so the shelf is walked once. Parts in
// stock outside any rack follow in one stop sorted by location.
BomPickPlan planBomPicks(const BomAnalysis& analysis, const InventoryStore& store);

// Page header of a stop: "Rack 1: take 26 parts from 7 slots".
VoiceLine bomPickStopTitle(const BomPickStop& stop);
// Line under the pick list: "Then Rack 2: 4 parts from 4 slots." or "That is the last stop."
VoiceLine bomPickNextStop(const BomPickPlan& plan, size_t stopIndex);
// Project summary under the project name: "You have 15 of 43 parts. The other 28 need ordering, 21
// have a DigiKey match."
VoiceLine bomProjectSummary(const BomAnalysis& analysis, int suggestedMatches);
// Finish screen title: "All 40 parts are picked" or "38 of 40 parts are picked".
VoiceLine bomPickFinishTitle(int taken, int planned);
// Finish screen line 2: "Deducting leaves 1uF and 10nF low. 28 lines are still on the shopping list."
VoiceLine bomPickFinishGuide(const std::vector<std::string>& lowAfter, size_t shortLines);

}  // namespace inventatory
