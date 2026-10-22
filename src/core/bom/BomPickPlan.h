// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

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
  size_t commitIndex = 0;  // position among the plan's picks, in route order
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
  // Matches stock cannot fully cover. Partly covered lines are also picked as far as stock allows.
  std::vector<size_t> shortMatches;
  // Candidates the user has not confirmed. Never picked from or deducted.
  std::vector<size_t> unconfirmedMatches;
  int pieces = 0;      // everything the stops ask for
  int pickLines = 0;   // BOM lines with at least one piece to take

  bool empty() const { return stops.empty(); }
};

// Rack stops first, in rack and slot order, so each shelf is walked once.
// Loose parts follow in one stop.
BomPickPlan planBomPicks(const BomAnalysis& analysis, const InventoryStore& store);

// A pick as committed in the walkthrough. Deducting needs the same item, fingerprint and stock.
struct BomPickCommit {
  std::string lineKey;
  std::string designation;  // the BOM value as written, for messages
  std::string itemId;
  int quantity = 0;  // most units this pick can take
  std::string fingerprint;
};

// In route order, so an index equals BomPick::commitIndex.
std::vector<BomPickCommit> bomPickCommits(const BomPickPlan& plan, const BomAnalysis& analysis,
                                          const InventoryStore& store);
bool sameBomPickCommits(const std::vector<BomPickCommit>& lhs, const std::vector<BomPickCommit>& rhs);
// One sentence per problem that blocks deducting; empty when every commit can apply.
std::vector<std::string> bomPickProblems(const std::vector<BomPickCommit>& commits, const std::vector<int>& taken,
                                         const InventoryStore& store);

// Page header of a stop: "Rack 1: take 26 pieces from 7 slots".
VoiceLine bomPickStopTitle(const BomPickStop& stop);
// Line under the pick list: "Then Rack 2: 4 pieces from 4 slots." or "That is the last stop."
VoiceLine bomPickNextStop(const BomPickPlan& plan, size_t stopIndex);
// Project summary under the project name: "You have 15 of 43 parts. The other 28 need ordering, 21
// have a supplier match."
VoiceLine bomProjectSummary(const BomAnalysis& analysis, int suggestedMatches);
// Finish screen title: "All 40 pieces are picked" or "38 of 40 pieces are picked".
VoiceLine bomPickFinishTitle(int taken, int planned);
// Finish screen line 2: "Deducting leaves 1uF and 10nF low. 28 lines are still on the shopping list."
VoiceLine bomPickFinishGuide(const std::vector<std::string>& lowAfter, size_t shortLines);

}  // namespace inventatory
