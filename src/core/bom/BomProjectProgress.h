// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - where a project stands on the way to a full build. Each BOM line is in one state, and
// the project page reads its counts from here, so no screen counts on its own.

#pragma once

#include "core/bom/BomMatch.h"
#include "core/inventory/Inventory.h"

#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

// One line's state. Every line not in stock goes ToChoose, Chosen, Ordered, then back to InStock once
// the order is received.
enum class BomLineState {
  InStock,    // confirmed stock covers the line
  ToConfirm,  // stock may have the part, and the user says yes or no
  ToChoose,   // stock does not cover it, and no part to buy is chosen
  Chosen,     // a part to buy is chosen, not ordered yet
  Ordered,    // ordered, waiting to be received
};

// A part ordered for a line, kept in the project's enrichment map under bomOrderKey.
struct BomOrder {
  std::string provider;  // kVendorProvider*
  std::string part;      // the part number the line's choice names
  std::uint64_t quantity = 0;
  std::time_t orderedAt = 0;
};

// "@order:<line key>": the "@" keeps it apart from every BOM line key.
std::string bomOrderKey(const std::string& lineKey);
std::string encodeBomOrder(const BomOrder& order);
// Nullopt for an incomplete value.
std::optional<BomOrder> decodeBomOrder(const std::string& value);
std::optional<BomOrder> bomLineOrder(const std::map<std::string, std::string>& enrichment, const std::string& lineKey);

// Pieces to buy for the line: what confirmed stock does not cover.
std::uint64_t bomQuantityToBuy(const BomMatch& match);

// Where a stock part is: "Rack 1  A3", its location text, or empty when it has neither.
std::string bomItemPlace(const InventoryItem& item, const std::vector<InventatoryRack>& racks);

BomLineState bomLineState(const BomMatch& match, const std::string& lineKey,
                          const std::map<std::string, std::string>& enrichment);

struct BomProjectProgress {
  int lines = 0;
  int inStock = 0;
  int inRacks = 0;      // in stock, with a rack slot
  int otherPlaces = 0;  // in stock, with a location outside the racks
  int noLocation = 0;   // in stock, with neither
  std::vector<std::string> places;            // the locations outside racks, each once, in BOM order
  std::vector<std::string> noLocationParts;   // the BOM values of the parts with no location
  int toConfirm = 0;
  int toChoose = 0;
  int chosen = 0;
  int ordered = 0;
  int notCovered() const { return lines - inStock; }
};

BomProjectProgress bomProjectProgress(const BomAnalysis& analysis, const std::map<std::string, std::string>& enrichment,
                                      const InventoryStore& store);

// Line 2 of the project page: "2 boards. 41 of 48 parts are in stock. 7 are not yet."
std::string bomOverviewSummary(const BomProjectProgress& progress, int boards);

// One row of the project page's steps.
struct BomStepRow {
  std::string name;      // "Confirm matches"
  std::string sentence;  // "2 parts may already be in stock."
  std::string count;     // "2 to do", "Done" or "-"
  bool open = false;     // the step has work left
};

// The four steps, in order: Confirm matches, Choose parts to buy, Order, Receive. `orderCost` is the
// chosen parts' total with its currency ("14.20 EUR"), empty when unknown; `orderStores` names their
// stores ("TME").
std::vector<BomStepRow> bomProjectSteps(const BomProjectProgress& progress, const std::string& orderCost,
                                        const std::string& orderStores);

}  // namespace inventatory
