// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System

#include "core/bom/BomPickPlan.h"

#include <algorithm>
#include <map>
#include <set>

namespace inventatory {

using namespace std;

namespace {

string listPhrase(const vector<string>& names) {
  string text;
  for (size_t index = 0; index < names.size(); ++index) {
    if (index > 0) text += index + 1 == names.size() ? " and " : ", ";
    text += names[index];
  }
  return text;
}

}  // namespace

int BomPickStop::pieces() const {
  int total = 0;
  for (const auto& pick : picks) total += pick.quantity;
  return total;
}

BomPickPlan planBomPicks(const BomAnalysis& analysis, const InventoryStore& store) {
  BomPickPlan plan;
  map<string, BomPickStop> rackStops;
  BomPickStop loose;
  loose.title = "Outside racks";
  // One stock item can serve several BOM lines, so later lines only get what earlier lines left.
  map<string, int> remaining;

  for (size_t index = 0; index < analysis.matches.size(); ++index) {
    const auto& match = analysis.matches[index];
    // Nothing in stock is a shortage. Unconfirmed matches wait and are never picked.
    if (match.candidates.empty()) {
      plan.shortMatches.push_back(index);
      continue;
    }
    if (!match.confirmed) {
      plan.unconfirmedMatches.push_back(index);
      continue;
    }
    if (!match.sufficient) plan.shortMatches.push_back(index);
    if (match.lineIndex >= analysis.lines.size()) continue;

    const auto* item = store.findById(match.chosenItemId());
    if (item != nullptr && remaining.count(item->id) == 0) remaining[item->id] = max(0, item->quantity);
    const int quantity = item == nullptr ? 0 : min(match.needed, remaining[item->id]);
    if (item != nullptr) remaining[item->id] -= max(0, quantity);
    // A line with nothing on hand is a shopping-list entry, never a stop on the route.
    if (quantity <= 0) continue;

    const auto& line = analysis.lines[match.lineIndex];
    BomPick pick;
    pick.matchIndex = index;
    pick.itemId = item->id;
    pick.designation = line.designation;
    pick.package = packageFromFootprint(line.footprint);
    pick.designators = line.designators;
    pick.needed = match.needed;
    pick.quantity = quantity;
    plan.pieces += quantity;
    ++plan.pickLines;

    const bool racked = !item->rackId.empty() && !item->rackSlot.empty() &&
                        any_of(store.racks().begin(), store.racks().end(),
                               [&](const InventatoryRack& rack) { return rack.id == item->rackId; });
    if (!racked) {
      pick.slot = item->location.empty() ? string("No location") : item->location;
      loose.picks.push_back(move(pick));
      continue;
    }
    pick.slot = item->rackSlot;
    auto& stop = rackStops[item->rackId];
    stop.rackId = item->rackId;
    stop.picks.push_back(move(pick));
  }

  vector<const InventatoryRack*> racks;
  for (const auto& rack : store.racks()) {
    if (rackStops.count(rack.id) != 0) racks.push_back(&rack);
  }
  sort(racks.begin(), racks.end(), [](const InventatoryRack* lhs, const InventatoryRack* rhs) {
    const auto left = rackNumberFromCode(lhs->code);
    const auto right = rackNumberFromCode(rhs->code);
    return left != right ? left < right : lhs->code < rhs->code;
  });

  const auto bySlot = [](const BomPick& lhs, const BomPick& rhs) {
    return lhs.slot != rhs.slot ? lhs.slot < rhs.slot : lhs.designation < rhs.designation;
  };
  for (const auto* rack : racks) {
    auto stop = move(rackStops[rack->id]);
    stop.title = "Rack " + to_string(max(1, rackNumberFromCode(rack->code)));
    stop.componentType = rack->componentType;
    sort(stop.picks.begin(), stop.picks.end(), bySlot);
    plan.stops.push_back(move(stop));
  }
  if (!loose.picks.empty()) {
    sort(loose.picks.begin(), loose.picks.end(), bySlot);
    plan.stops.push_back(move(loose));
  }

  // Commit indexes follow route order, so a deduction can refer to each pick by position.
  size_t commitIndex = 0;
  for (auto& stop : plan.stops) {
    for (auto& pick : stop.picks) pick.commitIndex = commitIndex++;
  }
  return plan;
}

vector<BomPickCommit> bomPickCommits(const BomPickPlan& plan, const BomAnalysis& analysis,
                                     const InventoryStore& store) {
  vector<BomPickCommit> commits;
  for (const auto& stop : plan.stops) {
    for (const auto& pick : stop.picks) {
      const auto& line = analysis.lines[analysis.matches[pick.matchIndex].lineIndex];
      const auto* item = store.findById(pick.itemId);
      BomPickCommit commit;
      commit.lineKey = bomLineKey(line);
      commit.designation = pick.designation;
      commit.itemId = pick.itemId;
      commit.quantity = pick.quantity;
      commit.fingerprint = item == nullptr ? string() : bomItemFingerprint(*item);
      commits.push_back(move(commit));
    }
  }
  return commits;
}

bool sameBomPickCommits(const vector<BomPickCommit>& lhs, const vector<BomPickCommit>& rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (size_t index = 0; index < lhs.size(); ++index) {
    const auto& left = lhs[index];
    const auto& right = rhs[index];
    if (left.lineKey != right.lineKey || left.itemId != right.itemId || left.quantity != right.quantity ||
        left.fingerprint != right.fingerprint) {
      return false;
    }
  }
  return true;
}

vector<string> bomPickProblems(const vector<BomPickCommit>& commits, const vector<int>& taken,
                               const InventoryStore& store) {
  vector<string> problems;
  // Picks may share one item, so sum the units drawn before comparing with stock.
  map<string, long long> drawn;
  for (size_t index = 0; index < commits.size(); ++index) {
    const int units = index < taken.size() ? taken[index] : commits[index].quantity;
    if (units > 0) drawn[commits[index].itemId] += units;
  }

  set<string> reportedItems;
  for (size_t index = 0; index < commits.size(); ++index) {
    const auto& commit = commits[index];
    const int units = index < taken.size() ? taken[index] : commit.quantity;
    if (units <= 0) continue;
    const auto* item = store.findById(commit.itemId);
    if (item == nullptr) {
      problems.push_back(commit.designation + " is no longer in stock.");
    } else if (bomItemFingerprint(*item) != commit.fingerprint) {
      problems.push_back(commit.designation + " changed since the walkthrough began.");
    } else if (drawn[commit.itemId] > item->quantity && reportedItems.insert(commit.itemId).second) {
      problems.push_back(item->partName + " has " + to_string(item->quantity) + " on hand, but the walkthrough takes " +
                         to_string(drawn[commit.itemId]) + ".");
    }
  }
  return problems;
}

VoiceLine bomPickStopTitle(const BomPickStop& stop) {
  const auto parts = voiceCount(stop.pieces(), "piece", "pieces");
  const auto slots = voiceCount(static_cast<int>(stop.picks.size()), stop.rackId.empty() ? "place" : "slot",
                                stop.rackId.empty() ? "places" : "slots");
  if (stop.rackId.empty()) return {{"Collect "}, {parts, VoiceTone::Strong}, {" kept outside racks"}};
  return {{stop.title, VoiceTone::Slot}, {": take "}, {parts, VoiceTone::Strong}, {" from "},
          {slots, VoiceTone::Strong}};
}

VoiceLine bomPickNextStop(const BomPickPlan& plan, size_t stopIndex) {
  if (stopIndex + 1 >= plan.stops.size()) return {{"This is the last stop."}};
  const auto& next = plan.stops[stopIndex + 1];
  const auto parts = voiceCount(next.pieces(), "piece", "pieces");
  if (next.rackId.empty()) return {{"Then "}, {parts, VoiceTone::Strong}, {" kept outside racks."}};
  return {{"Then "},
          {next.title, VoiceTone::Slot},
          {": " + parts + " from " + voiceCount(static_cast<int>(next.picks.size()), "slot.", "slots.")}};
}

VoiceLine bomProjectSummary(const BomAnalysis& analysis, int suggestedMatches) {
  const int lines = static_cast<int>(analysis.matches.size());
  if (lines == 0) return {{"This BOM has no parts to place."}};
  const auto boards = voiceCount(analysis.boards, "board", "boards");
  if (analysis.shortCount == 0 && analysis.confirmCount == 0) {
    return {{"You have all "}, {to_string(lines), VoiceTone::Strong}, {" parts for " + boards + "."}};
  }
  if (analysis.shortCount == 0) {
    return {{"You have "}, {to_string(analysis.readyCount) + " of " + to_string(lines), VoiceTone::Strong},
            {" parts. "}, {to_string(analysis.confirmCount), VoiceTone::Strong},
            {analysis.confirmCount == 1 ? " needs your confirmation." : " need your confirmation."}};
  }
  VoiceLine line;
  if (analysis.readyCount == 0) {
    line = {{"None of the "}, {to_string(lines), VoiceTone::Strong}, {" parts are in stock"}};
  } else {
    line = {{"You have "},
            {to_string(analysis.readyCount) + " of " + to_string(lines), VoiceTone::Strong},
            {" parts. The other "},
            {to_string(analysis.shortCount), VoiceTone::Danger},
            {analysis.shortCount == 1 ? " needs ordering" : " need ordering"}};
  }
  if (analysis.confirmCount > 0) {
    line.push_back({", "});
    line.push_back({to_string(analysis.confirmCount), VoiceTone::Strong});
    line.push_back({analysis.confirmCount == 1 ? " needs confirming" : " need confirming"});
  }
  if (suggestedMatches > 0) {
    line.push_back({", "});
    line.push_back({to_string(suggestedMatches), VoiceTone::Strong});
    line.push_back({suggestedMatches == 1 ? " has a supplier match." : " have a supplier match."});
  } else {
    line.push_back({"."});
  }
  return line;
}

VoiceLine bomPickFinishTitle(int taken, int planned) {
  if (taken == planned) return {{"All "}, {voiceCount(planned, "piece is", "pieces are"), VoiceTone::Strong}, {" picked"}};
  return {{to_string(taken) + " of " + voiceCount(planned, "piece is", "pieces are"), VoiceTone::Strong}, {" picked"}};
}

VoiceLine bomPickFinishGuide(const vector<string>& lowAfter, size_t shortLines) {
  VoiceLine line;
  if (lowAfter.empty()) {
    line.push_back({"Nothing runs low after deducting."});
  } else {
    line.push_back({"Deducting leaves "});
    line.push_back({lowAfter.size() <= 3 ? listPhrase(lowAfter) : voiceCount(static_cast<int>(lowAfter.size()), "part", "parts"),
                    VoiceTone::Warning});
    line.push_back({" low."});
  }
  if (shortLines > 0) {
    line.push_back({" "});
    line.push_back({voiceCount(static_cast<int>(shortLines), "line", "lines"), VoiceTone::Danger});
    line.push_back({shortLines == 1 ? " is still on the shopping list." : " are still on the shopping list."});
  }
  return line;
}

}  // namespace inventatory
