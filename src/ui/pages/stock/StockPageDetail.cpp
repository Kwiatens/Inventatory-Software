// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - selected stock item detail and editor rendering.

#include "App.h"

#include "core/inventory/VendorProvider.h"
#include "core/parts/PartDescriptor.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace inventatory {

using namespace std;

ftxui::Elements App::renderStockDetailRows(const vector<InventorySearchMatch>& searchMatches,
                                            bool rankedView, size_t activeSelection,
                                            int detailInnerWidth) const {
  const auto fixedCell = [](const string& text, int width, ftxui::Color color) {
    return ftxui::hbox({styledText(ellipsize(text, static_cast<size_t>(max(width, 0))), color), ftxui::filler()}) |
           ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
  };
  const auto bandColor = [](PhysicalValueMatchBand band) {
    switch (band) {
      case PhysicalValueMatchBand::Exact: return uiSuccessColor();
      case PhysicalValueMatchBand::Workable: return uiFocusColor();
      case PhysicalValueMatchBand::Possible: return uiWarnColor();
      case PhysicalValueMatchBand::None: return uiMutedColor();
    }
    return uiMutedColor();
  };

  // Editing renders inline: the detail panel becomes the form, with live typing in place.
  const bool editing = inputMode_ == InputMode::EditFieldMenu || inputMode_ == InputMode::EditValue;
  const InventorySearchMatch* selectedMatch = nullptr;
  if (rankedView && !searchMatches.empty()) {
    const auto selectedPosition = min(activeSelection, searchMatches.size() - 1);
    selectedMatch = &searchMatches[selectedPosition];
  }

  ftxui::Elements detailRows;
  auto self = const_cast<App*>(this);
  if (inputMode_ == InputMode::VendorPick) {
    // One block per vendor that answered; the highlighted one previews what it would fill in.
    const auto width = static_cast<size_t>(max(detailInnerWidth, 8));
    detailRows.push_back(fullLine(ellipsize("Vendor for " + vendorLookupQuery_, width), uiTitleColor(), uiRowSelectedBg()));
    detailRows.push_back(uiDivider());
    for (size_t index = 0; index < vendorCandidates_.size(); ++index) {
      const auto& candidate = vendorCandidates_[index];
      const bool selected = index == vendorCandidateIndex_;
      const string vendorName = vendorDisplayName(candidate.provider);
      string facts;
      for (const auto& fact : {candidate.manufacturer, candidate.packageName, candidate.stock, candidate.price}) {
        if (trim(fact).empty()) continue;
        facts += (facts.empty() ? "" : ", ") + trim(fact);
      }
      auto row = ftxui::vbox({
                     styledText(ellipsize(vendorName + "  " + candidate.partNumber, width),
                                selected ? uiTitleColor() : uiPrimaryText()),
                     styledText(ellipsize(facts, width), uiMutedColor()),
                 }) |
                 ftxui::bgcolor(selected ? uiSelectionBg() : uiSurfaceBg());
      if (selected) {
        row = row | ftxui::select;
      }
      detailRows.push_back(target(row, "stock.vendor." + to_string(index), UiTargetKind::Button,
                                  [self, index] { self->chooseVendorCandidate(index); }));
    }
    if (!vendorCandidates_.empty()) {
      const auto& highlighted = vendorCandidates_[min(vendorCandidateIndex_, vendorCandidates_.size() - 1)];
      vector<string> fills;
      if (trim(workingCopy_.item.partName).empty() || workingCopy_.item.partName == "New Part") fills.push_back("part name");
      if (trim(workingCopy_.item.manufacturer).empty() || workingCopy_.item.manufacturer == "Unknown") {
        fills.push_back("manufacturer");
      }
      if (!highlighted.packageName.empty()) fills.push_back("package");
      if (highlighted.parameterCount > 0) fills.push_back(to_string(highlighted.parameterCount) + " parameters");
      if (highlighted.hasDatasheet) fills.push_back("datasheet");
      if (!highlighted.price.empty()) fills.push_back("price");
      string fillLine = "Fills in: ";
      for (size_t index = 0; index < fills.size(); ++index) {
        fillLine += (index == 0 ? "" : ", ") + fills[index];
      }
      if (fills.empty()) fillLine += "nothing new";
      detailRows.push_back(styledText(ellipsize(fillLine, width), uiSecondaryText()));
    }
    for (const auto& note : vendorLookupNotes_) {
      detailRows.push_back(styledText(ellipsize(note, width), uiSecondaryText()));
    }
    detailRows.push_back(styledText(ellipsize("Up/Down choose   Enter use this vendor   Esc back", width), uiMutedColor()));
  } else if (editing) {
    const auto titleText = workingCopy_.isNew ? string("New part") : workingCopy_.item.partName;
    detailRows.push_back(fullLine(titleText, uiTitleColor(), uiRowSelectedBg()));
    detailRows.push_back(uiDivider());
    const int labelWidth = clamp(detailInnerWidth / 3, 14, 20);
    for (size_t index = 0; index < menuOptions_.size(); ++index) {
      const auto& option = menuOptions_[index];
      const bool selected = static_cast<int>(index) == fieldMenuIndex_;
      const bool typing = selected && inputMode_ == InputMode::EditValue;
      const auto value = typing ? inputBuffer_ + "_" : currentFieldValue(option.field);
      const auto bg = selected ? uiSelectionBg() : uiSurfaceBg();
      const auto labelColor = selected ? uiTitleColor() : uiMutedColor();
      const auto valueColor = typing ? uiLinkColor() : (selected ? uiTitleColor() : uiLabelColor());
      auto row = ftxui::hbox({
                     fixedCell(option.label, labelWidth, labelColor),
                     styledText(ellipsize(value, static_cast<size_t>(max(detailInnerWidth - labelWidth, 4))),
                                valueColor),
                     ftxui::filler(),
                 }) |
                 ftxui::bgcolor(bg);
      if (selected) {
        row = row | ftxui::select;
      }
      detailRows.push_back(target(row, "stock.field." + to_string(index), UiTargetKind::Field,
                                  [self, index] {
                                    self->fieldMenuIndex_ = static_cast<int>(index);
                                    if (self->menuOptions_[index].field == EditField::VendorSource) {
                                      self->beginVendorLookup();
                                      return;
                                    }
                                    self->inputBuffer_ = self->currentFieldValue(self->menuOptions_[index].field);
                                    self->inputMode_ = InputMode::EditValue;
                                    self->dirty_ = true;
                                  }));
    }
  } else if (const auto* item = selectedItem()) {
    const auto electricalFields = electricalFieldsForItem(*item);
    const bool passive = categoryContains(*item, {"resistor", "capacitor", "inductor", "diode", "fuse",
                                                   "thermistor", "varistor", "crystal", "resonator"});
    const auto manufacturer = trim(item->manufacturer).empty() ? string("Unknown manufacturer") : item->manufacturer;
    detailRows.push_back(ftxui::paragraphAlignLeft(item->partName) | ftxui::bold | ftxui::color(uiPrimaryText()));
    detailRows.push_back(styledText(manufacturer, uiMutedColor()));
    if (passive) {
      ftxui::Elements summary;
      for (size_t index = 0; index < electricalFields.size() && index < 3; ++index) {
        if (trim(electricalFields[index].value).empty()) continue;
        summary.push_back(styledText(summary.empty() ? string() : "  ", uiSecondaryText()));
        summary.push_back(styledText(electricalFields[index].value, uiSecondaryText()));
      }
      if (!summary.empty()) detailRows.push_back(ftxui::hbox(move(summary)));
    }
    if (!trim(partShortDescription(*item)).empty() && partShortDescription(*item) != item->partName) {
      detailRows.push_back(styledText(partShortDescription(*item), uiMutedColor()));
    }

    // Qty and rack sit under the summary, not in the details list.
    const auto rack = rackLocation(*item, store_.racks());
    const auto quantityColor = uiQuantityColor(*item, settings_.lowStockThreshold);
    const bool outOfStock = item->quantity <= 0;
    const bool lowStock = !outOfStock && isLowStock(*item, settings_.lowStockThreshold);
    detailRows.push_back(uiDivider());
    ftxui::Elements essentials = {
        styledText(" Qty ", uiMutedColor(), uiRaisedSurfaceBg()),
        uiHeaderText(to_string(item->quantity), quantityColor, uiRaisedSurfaceBg()),
    };
    if (outOfStock || lowStock) {
      essentials.push_back(uiHeaderText(outOfStock ? "  OUT" : "  LOW", quantityColor, uiRaisedSurfaceBg()));
    }
    essentials.push_back(styledText("     Rack ", uiMutedColor(), uiRaisedSurfaceBg()));
    essentials.push_back(uiHeaderText(rack.empty() ? "NOT ASSIGNED" : rack, rack.empty() ? uiWarnColor() : uiFocusColor(),
                                      uiRaisedSurfaceBg()));
    essentials.push_back(ftxui::filler());
    detailRows.push_back(ftxui::hbox(move(essentials)) | ftxui::bgcolor(uiRaisedSurfaceBg()));

    if (selectedMatch != nullptr) {
      const auto targetText = closestSearchActive_ ? closestSearchQuery_ : searchQuery_;
      const auto matchText = selectedMatch->band == PhysicalValueMatchBand::Exact
                                 ? string("Exact normalized value")
                                 : selectedMatch->hasPhysicalComparison
                                       ? string(physicalValueMatchBandName(selectedMatch->band))
                                       : string("Text match");
      ostringstream distance;
      if (selectedMatch->hasPhysicalComparison && selectedMatch->band != PhysicalValueMatchBand::Exact &&
          isfinite(selectedMatch->signedRelativeDifference)) {
        distance << showpos << fixed << setprecision(1) << selectedMatch->signedRelativeDifference * 100.0 << "%";
      }
      detailRows.push_back(uiDivider());
      detailRows.push_back(fullLine("Match", bandColor(selectedMatch->band), uiSurfaceBg()));
      detailRows.push_back(detailFieldLine({"Fit: ", matchText, uiSecondaryText(), bandColor(selectedMatch->band)},
                                            detailInnerWidth));
      if (!distance.str().empty()) {
        detailRows.push_back(detailFieldLine({"Delta: ", distance.str(), uiSecondaryText(), uiPrimaryText()},
                                              detailInnerWidth));
      }
      if (!targetText.empty()) {
        detailRows.push_back(detailFieldLine({"Target: ", targetText, uiSecondaryText(), uiPrimaryText()},
                                              detailInnerWidth));
      }
    }

    if (stocktakeActive_) {
      const auto counted = stocktakeCounts_.find(item->id);
      const bool alreadyCounted = counted != stocktakeCounts_.end();
      const auto physicalCount = stocktakeCountFor(*item);
      detailRows.push_back(uiDivider());
      detailRows.push_back(fullLine("Stocktake count", uiAccentColor(), uiSurfaceBg()));
      detailRows.push_back(ftxui::hbox({
          styledText(" System ", uiSecondaryText()),
          uiBodyText(to_string(item->quantity), uiPrimaryText()),
          styledText("     Count ", uiSecondaryText()),
          uiBodyText(to_string(physicalCount), alreadyCounted ? uiFocusColor() : uiWarnColor()),
          ftxui::filler(),
      }));
      detailRows.push_back(fullLine(inputMode_ == InputMode::StocktakeCount
                                        ? "Enter the physical count, then press Enter"
                                        : "Press Enter to count this part, s to finish, q to cancel",
                                    uiMutedColor(), uiSurfaceBg()));
      if (inputMode_ == InputMode::StocktakeCount) {
        detailRows.push_back(fullLine(" Count: " + inputBuffer_ + "_", uiLinkColor(), uiSelectionBg()));
      }
    }

    vector<const InventoryMovement*> recentMovements;
    for (const auto& movement : inventoryMovements_) {
      if (movement.itemId == item->id) {
        recentMovements.push_back(&movement);
        if (recentMovements.size() == 5) break;
      }
    }
    if (!recentMovements.empty()) {
      detailRows.push_back(uiDivider());
      detailRows.push_back(fullLine("Recent movements", uiSecondaryText(), uiSurfaceBg()));
      for (const auto* movement : recentMovements) {
        const auto delta = movement->delta > 0 ? "+" + to_string(movement->delta) : to_string(movement->delta);
        auto reference = movement->reference.empty() ? string() : ", " + movement->reference;
        const auto summary = delta + ", " + movement->source + reference + ", " +
                             to_string(movement->quantityBefore) + " to " + to_string(movement->quantityAfter) +
                             ", " + nowTimestampString(movement->occurredAt);
        detailRows.push_back(fullLine(ellipsize(summary, static_cast<size_t>(detailInnerWidth)),
                                      movement->delta < 0 ? uiWarnColor() : uiFocusColor(), uiSurfaceBg()));
      }
    }

    if (!electricalFields.empty()) {
      detailRows.push_back(uiDivider());
      detailRows.push_back(fullLine("Electrical parameters", uiSecondaryText(), uiSurfaceBg()));
      for (const auto& field : electricalFields) {
        // The passive summary already shows the package.
        if (passive && field.label == "Package: ") continue;
        detailRows.push_back(detailFieldLine(field, detailInnerWidth));
      }
    }

    // Empty fields are dropped, not shown as a dash.
    ftxui::Elements detailRowsExpanded;
    const auto detailField = [&](const string& label, const string& value, ftxui::Color valueColor) {
      if (trim(value).empty()) return;
      detailRowsExpanded.push_back(detailFieldLine({label + ": ", value, uiSecondaryText(), valueColor}, detailInnerWidth));
    };
    detailField("DigiKey", item->digikeyPartNumber, uiPrimaryText());
    detailField("TME", tmeProductSymbol(*item), uiPrimaryText());
    detailField("Mouser", mouserPartNumberOf(*item), uiPrimaryText());
    detailField("SKU", item->sku, uiPrimaryText());
    detailField("Inventatory ID", item->inventatoryId, uiPrimaryText());
    detailField("Location", item->location, uiPrimaryText());
    detailField("Tags", renderTags(item->tags), uiPrimaryText());
    // The heading shows how many fields are hidden, so it never looks like missing data.
    detailRows.push_back(uiDivider());
    detailRows.push_back(target(ftxui::hbox({
                                    styledText(stockDetailsExpanded_ ? " - Details" : " + Details",
                                               uiSecondaryText()),
                                    ftxui::filler(),
                                    styledText(stockDetailsExpanded_
                                                   ? string()
                                                   : to_string(detailRowsExpanded.size()) + " fields ",
                                               uiMutedText()),
                                }) | ftxui::bgcolor(uiSurfaceBg()),
                                "stock.details.toggle", UiTargetKind::Button, [self] {
                                  self->stockDetailsExpanded_ = !self->stockDetailsExpanded_;
                                  self->dirty_ = true;
                                }));
    if (stockDetailsExpanded_) {
      for (auto& row : detailRowsExpanded) detailRows.push_back(move(row));
    }

    ftxui::Elements linkRows;
    const auto link = [&](const string& id, const string& label, const string& value, function<void()> activate) {
      if (trim(value).empty()) return;
      linkRows.push_back(target(detailFieldLine({label + ": ", renderUrl(value), uiSecondaryText(), uiInteractiveColor()},
                                                detailInnerWidth),
                                id, UiTargetKind::Link, move(activate)));
    };
    link("stock.link.datasheet", "Datasheet", item->datasheetUrl,
         [self] { if (const auto* value = self->selectedItem()) self->openCurrentUrl(value->datasheetUrl, "datasheet"); });
    link("stock.link.product", "Product", item->productUrl,
         [self] { if (const auto* value = self->selectedItem()) self->openCurrentUrl(value->productUrl, "product"); });
    if (!linkRows.empty()) {
      detailRows.push_back(uiDivider());
      detailRows.push_back(fullLine("Links", uiSecondaryText(), uiSurfaceBg()));
      for (auto& row : linkRows) detailRows.push_back(move(row));
    }

    if (!trim(item->notes).empty()) {
      detailRows.push_back(uiDivider());
      detailRows.push_back(fullLine("Notes", uiSecondaryText(), uiSurfaceBg()));
      detailRows.push_back(ftxui::paragraphAlignLeft(item->notes) | ftxui::color(uiPrimaryText()));
    }
  } else {
    detailRows.push_back(fullLine("No item selected.", uiMutedColor(), uiPanelRightBg()));
  }
  return detailRows;
}

}  // namespace inventatory
