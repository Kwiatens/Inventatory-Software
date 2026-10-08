// Inventatory - Hardware Inventory Management System
// Find in racks: the rack-by-rack pick route for a BOM analysis, and the sentences that guide it.

#include "core/bom/BomPickPlan.h"

#include <algorithm>
#include <map>

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
  return plan;
}

VoiceLine bomPickStopTitle(const BomPickStop& stop) {
  const auto parts = voiceCount(stop.pieces(), "part", "parts");
  const auto slots = voiceCount(static_cast<int>(stop.picks.size()), stop.rackId.empty() ? "place" : "slot",
                                stop.rackId.empty() ? "places" : "slots");
  if (stop.rackId.empty()) return {{"Collect "}, {parts, VoiceTone::Strong}, {" kept outside racks"}};
  return {{"Open "}, {stop.title, VoiceTone::Slot}, {" and take "}, {parts, VoiceTone::Strong}, {" from "},
          {slots, VoiceTone::Strong}};
}

VoiceLine bomPickStopGuide(const BomPickStop& stop) {
  if (stop.picks.empty()) return {};
  if (stop.picks.size() == 1) {
    const auto& pick = stop.picks.front();
    return {{"Take "},
            {to_string(pick.quantity) + " of " + pick.designation, VoiceTone::Strong},
            {" from "},
            {pick.slot, VoiceTone::Slot},
            {"."}};
  }

  vector<const BomPick*> ranked;
  for (const auto& pick : stop.picks) ranked.push_back(&pick);
  stable_sort(ranked.begin(), ranked.end(),
              [](const BomPick* lhs, const BomPick* rhs) { return lhs->quantity > rhs->quantity; });

  if (ranked.front()->quantity == 1) return {{"One part from each "}, {stop.rackId.empty() ? "place." : "slot."}};

  VoiceLine line{{"Start with "}};
  size_t mentioned = 0;
  for (const auto* pick : ranked) {
    if (mentioned == 2 || pick->quantity < 2) break;
    if (mentioned == 1) line.push_back({" and "});
    line.push_back({to_string(pick->quantity) + " of " + pick->designation, VoiceTone::Strong});
    line.push_back({" from "});
    line.push_back({pick->slot, VoiceTone::Slot});
    ++mentioned;
  }
  line.push_back({"."});

  const auto rest = ranked.size() - mentioned;
  if (rest == 0) return line;
  int largestRest = 0;
  for (size_t index = mentioned; index < ranked.size(); ++index) largestRest = max(largestRest, ranked[index]->quantity);
  if (largestRest == 1) line.push_back({rest == 1 ? " The last one is a single part." : " The rest are one each."});
  else if (largestRest == 2) line.push_back({" The rest are one or two each."});
  else line.push_back({" Then " + voiceCount(static_cast<int>(rest), "more slot.", "more slots.")});
  return line;
}

VoiceLine bomPickNextStop(const BomPickPlan& plan, size_t stopIndex) {
  if (stopIndex + 1 >= plan.stops.size()) return {{"This is the last stop."}};
  const auto& next = plan.stops[stopIndex + 1];
  const auto parts = voiceCount(next.pieces(), "part", "parts");
  if (next.rackId.empty()) return {{"Then "}, {parts, VoiceTone::Strong}, {" kept outside racks."}};
  return {{"Then "},
          {next.title, VoiceTone::Slot},
          {": " + parts + " from " + voiceCount(static_cast<int>(next.picks.size()), "slot.", "slots.")}};
}

VoiceLine bomProjectSummary(const BomAnalysis& analysis, int suggestedMatches) {
  const int lines = static_cast<int>(analysis.matches.size());
  if (lines == 0) return {{"This BOM has no parts to place."}};
  const auto boards = voiceCount(analysis.boards, "board", "boards");
  if (analysis.shortCount == 0) {
    return {{"You have all "}, {to_string(lines), VoiceTone::Strong}, {" parts for " + boards + "."}};
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
  if (suggestedMatches > 0) {
    line.push_back({", "});
    line.push_back({to_string(suggestedMatches), VoiceTone::Strong});
    line.push_back({suggestedMatches == 1 ? " has a DigiKey match." : " have a DigiKey match."});
  } else {
    line.push_back({"."});
  }
  return line;
}

VoiceLine bomPickFinishTitle(int taken, int planned) {
  if (taken == planned) return {{"All "}, {voiceCount(planned, "part is", "parts are"), VoiceTone::Strong}, {" picked"}};
  return {{to_string(taken) + " of " + voiceCount(planned, "part is", "parts are"), VoiceTone::Strong}, {" picked"}};
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
