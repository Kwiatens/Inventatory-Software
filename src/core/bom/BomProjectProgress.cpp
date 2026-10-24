// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#include "core/bom/BomProjectProgress.h"

#include "core/bom/BomProjectStore.h"
#include "core/text/Voice.h"

#include <algorithm>

namespace inventatory {

using namespace std;

namespace {

constexpr char kOrderSeparator = '\x1f';

vector<string> splitFields(const string& value) {
  vector<string> fields;
  size_t start = 0;
  while (true) {
    const auto end = value.find(kOrderSeparator, start);
    fields.push_back(value.substr(start, end == string::npos ? string::npos : end - start));
    if (end == string::npos) break;
    start = end + 1;
  }
  return fields;
}

// "1 part", "3 parts": every count on the project page counts BOM lines.
string parts(int count) { return voiceCount(count, "part", "parts"); }

}  // namespace

string bomOrderKey(const string& lineKey) { return "@order:" + lineKey; }

string encodeBomOrder(const BomOrder& order) {
  return order.provider + kOrderSeparator + order.part + kOrderSeparator + to_string(order.quantity) + kOrderSeparator +
         to_string(static_cast<long long>(order.orderedAt));
}

optional<BomOrder> decodeBomOrder(const string& value) {
  const auto fields = splitFields(value);
  if (fields.size() != 4 || fields[0].empty() || fields[1].empty()) return nullopt;
  BomOrder order;
  order.provider = fields[0];
  order.part = fields[1];
  try {
    size_t used = 0;
    order.quantity = stoull(fields[2], &used);
    if (used != fields[2].size()) return nullopt;
    order.orderedAt = static_cast<time_t>(stoll(fields[3], &used));
    if (used != fields[3].size()) return nullopt;
  } catch (...) {
    return nullopt;
  }
  return order;
}

optional<BomOrder> bomLineOrder(const map<string, string>& enrichment, const string& lineKey) {
  const auto found = enrichment.find(bomOrderKey(lineKey));
  if (found == enrichment.end()) return nullopt;
  return decodeBomOrder(found->second);
}

uint64_t bomQuantityToBuy(const BomMatch& match) {
  const int covered = match.confirmed ? match.available : 0;
  return static_cast<uint64_t>(max(0, match.needed - covered));
}

string bomItemPlace(const InventoryItem& item, const vector<InventatoryRack>& racks) {
  const auto rack = rackLocation(item, racks);
  if (rack.empty()) return trim(item.location);
  // rackLocation gives "R1-E3"; the rack is spelled out as the pick route does.
  const auto dash = rack.find('-');
  if (dash == string::npos) return rack;
  return "Rack " + to_string(max(1, rackNumberFromCode(rack.substr(0, dash)))) + "  " + rack.substr(dash + 1);
}

BomLineState bomLineState(const BomMatch& match, const string& lineKey, const map<string, string>& enrichment) {
  if (match.sufficient) return BomLineState::InStock;
  if (match.needsConfirmation()) return BomLineState::ToConfirm;
  if (bomLineOrder(enrichment, lineKey).has_value()) return BomLineState::Ordered;
  const auto found = enrichment.find(lineKey);
  if (found != enrichment.end() && bomEnrichmentChosen(found->second) && !bomEnrichmentPart(found->second).empty()) {
    return BomLineState::Chosen;
  }
  return BomLineState::ToChoose;
}

BomProjectProgress bomProjectProgress(const BomAnalysis& analysis, const map<string, string>& enrichment,
                                      const InventoryStore& store) {
  BomProjectProgress progress;
  progress.lines = static_cast<int>(analysis.matches.size());
  for (const auto& match : analysis.matches) {
    const auto& line = analysis.lines[match.lineIndex];
    switch (bomLineState(match, bomLineKey(line), enrichment)) {
      case BomLineState::InStock: {
        ++progress.inStock;
        const auto* item = store.findById(match.chosenItemId());
        if (item != nullptr && !rackLocation(*item, store.racks()).empty()) {
          ++progress.inRacks;
        } else if (item != nullptr && !trim(item->location).empty()) {
          ++progress.otherPlaces;
          const auto place = trim(item->location);
          if (find(progress.places.begin(), progress.places.end(), place) == progress.places.end()) {
            progress.places.push_back(place);
          }
        } else {
          ++progress.noLocation;
          progress.noLocationParts.push_back(trim(line.designation + " " + packageFromFootprint(line.footprint)));
        }
        break;
      }
      case BomLineState::ToConfirm: ++progress.toConfirm; break;
      case BomLineState::ToChoose: ++progress.toChoose; break;
      case BomLineState::Chosen: ++progress.chosen; break;
      case BomLineState::Ordered: ++progress.ordered; break;
    }
  }
  return progress;
}

string bomOverviewSummary(const BomProjectProgress& progress, int boards) {
  const auto boardText = voiceCount(max(1, boards), "board", "boards") + ". ";
  if (progress.lines == 0) return boardText + "This BOM has no parts.";
  if (progress.notCovered() == 0) {
    return boardText + (progress.lines == 1 ? "The 1 part is in stock." : "All " + parts(progress.lines) + " are in stock.");
  }
  if (progress.inStock == 0) {
    return boardText + (progress.lines == 1 ? "The 1 part is not in stock yet." : "None of the " + parts(progress.lines) + " are in stock yet.");
  }
  return boardText + to_string(progress.inStock) + " of " + parts(progress.lines) + " are in stock. " +
         to_string(progress.notCovered()) + (progress.notCovered() == 1 ? " is not yet." : " are not yet.");
}

vector<BomStepRow> bomProjectSteps(const BomProjectProgress& progress, const string& orderCost, const string& orderStores) {
  vector<BomStepRow> rows;
  const auto todo = [](int count) { return to_string(count) + " to do"; };

  BomStepRow confirm{"Confirm matches", "Nothing to confirm.", "Done", false};
  if (progress.toConfirm > 0) {
    confirm.sentence = parts(progress.toConfirm) + " may already be in stock.";
    confirm.count = todo(progress.toConfirm);
    confirm.open = true;
  }
  rows.push_back(confirm);

  BomStepRow choose{"Choose parts to buy", "Nothing to buy.", "Done", false};
  const int buying = progress.toChoose + progress.chosen;
  if (buying > 0) {
    choose.sentence = parts(buying) + (buying == 1 ? " is not in stock." : " are not in stock.");
    if (progress.chosen > 0) {
      choose.sentence += " " + to_string(progress.chosen) + " chosen" + (orderStores.empty() ? string() : " at " + orderStores) + ".";
    }
    choose.count = progress.toChoose > 0 ? todo(progress.toChoose) : "Done";
    choose.open = progress.toChoose > 0;
  } else if (progress.ordered > 0) {
    choose.sentence = "Every part to buy is ordered.";
  }
  rows.push_back(choose);

  BomStepRow order{"Order", "Nothing chosen to order.", "-", false};
  if (progress.chosen > 0) {
    order.sentence = parts(progress.chosen) + " chosen";
    if (!orderCost.empty()) order.sentence += ", about " + orderCost;
    if (!orderStores.empty()) order.sentence += " at " + orderStores;
    order.sentence += ".";
    order.count = todo(progress.chosen);
    order.open = true;
  }
  rows.push_back(order);

  BomStepRow receive{"Receive", "Nothing ordered yet.", "-", false};
  if (progress.ordered > 0) {
    receive.sentence = parts(progress.ordered) + " ordered, not received yet.";
    receive.count = todo(progress.ordered);
    receive.open = true;
  }
  rows.push_back(receive);
  return rows;
}

}  // namespace inventatory
