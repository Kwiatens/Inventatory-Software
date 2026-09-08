// Inventatory - selected stock item detail and editor rendering.

#include "App.h"

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

  // Editing renders inline here instead of a separate blocking field-name menu:
  // the Detail panel becomes the form, current field highlighted, live typing
  // shown in place. Save ('s') and cancel (Esc) act on the whole form.
  const bool editing = inputMode_ == InputMode::EditFieldMenu || inputMode_ == InputMode::EditValue;
  const InventorySearchMatch* selectedMatch = nullptr;
  if (rankedView && !searchMatches.empty()) {
    const auto selectedPosition = min(activeSelection, searchMatches.size() - 1);
    selectedMatch = &searchMatches[selectedPosition];
  }

  ftxui::Elements detailRows;
  auto self = const_cast<App*>(this);
  if (editing) {
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
                                    self->inputBuffer_ = self->currentFieldValue(self->menuOptions_[index].field);
                                    self->inputMode_ = InputMode::EditValue;
                                    self->dirty_ = true;
                                  }));
    }
  } else if (const auto* item = selectedItem()) {
    const auto electricalFields = electricalFieldsForItem(*item);
    const bool passive = categoryContains(*item, {"resistor", "capacitor", "inductor", "diode", "fuse",
                                                   "thermistor", "varistor", "crystal", "resonator"});
    const auto manufacturer = trim(item->manufacturer).empty() ? string("UNKNOWN MANUFACTURER") : item->manufacturer;
    detailRows.push_back(uiBodyText(manufacturer, uiPrimaryText()));
    if (!passive) {
      detailRows.push_back(styledText(item->partName, uiSecondaryText()));
    } else {
      const auto primary = electricalFields.empty() ? item->partName : electricalFields.front().value;
      ftxui::Elements summary = {uiBodyText(primary, uiPrimaryText())};
      for (size_t index = 1; index < electricalFields.size() && index < 3; ++index) {
        if (trim(electricalFields[index].value).empty()) continue;
        summary.push_back(styledText("  " + electricalFields[index].value, uiSecondaryText()));
      }
      detailRows.push_back(ftxui::hbox(move(summary)));
    }
    if (!trim(partShortDescription(*item)).empty() && partShortDescription(*item) != item->partName) {
      detailRows.push_back(styledText(partShortDescription(*item), uiMutedColor()));
    }

    // Essentials: the two facts the footer buttons act on, kept directly under
    // the summary instead of at the bottom of a details list.
    const auto rack = rackLocation(*item, store_.racks());
    const auto quantityColor = item->quantity <= 0 ? uiDangerColor()
                               : isLowStock(*item, settings_.lowStockThreshold) ? uiWarnColor()
                                                  : uiPrimaryText();
    detailRows.push_back(uiDivider());
    detailRows.push_back(ftxui::hbox({
        styledText(" Qty ", uiSecondaryText()),
        uiBodyText(to_string(item->quantity), quantityColor),
        styledText("     Rack ", uiSecondaryText()),
        uiBodyText(rack.empty() ? "NOT ASSIGNED" : rack, rack.empty() ? uiWarnColor() : uiFocusColor()),
        ftxui::filler(),
    }));

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
      detailRows.push_back(fullLine("MATCH", bandColor(selectedMatch->band), uiSurfaceBg()));
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
      detailRows.push_back(fullLine("STOCKTAKE COUNT", uiAccentColor(), uiSurfaceBg()));
      detailRows.push_back(ftxui::hbox({
          styledText(" System ", uiSecondaryText()),
          uiBodyText(to_string(item->quantity), uiPrimaryText()),
          styledText("     Count ", uiSecondaryText()),
          uiBodyText(to_string(physicalCount), alreadyCounted ? uiFocusColor() : uiWarnColor()),
          ftxui::filler(),
      }));
      detailRows.push_back(fullLine(inputMode_ == InputMode::StocktakeCount
                                        ? "Enter the physical count, then press Enter"
                                        : "Press Enter to count this part · s finish · q cancel",
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
      detailRows.push_back(fullLine("RECENT MOVEMENTS", uiSecondaryText(), uiSurfaceBg()));
      for (const auto* movement : recentMovements) {
        const auto delta = movement->delta > 0 ? "+" + to_string(movement->delta) : to_string(movement->delta);
        auto reference = movement->reference.empty() ? string() : " · " + movement->reference;
        const auto summary = delta + " · " + movement->source + reference + " · " +
                             to_string(movement->quantityBefore) + " → " + to_string(movement->quantityAfter) +
                             " · " + nowTimestampString(movement->occurredAt);
        detailRows.push_back(fullLine(ellipsize(summary, static_cast<size_t>(detailInnerWidth)),
                                      movement->delta < 0 ? uiWarnColor() : uiFocusColor(), uiSurfaceBg()));
      }
    }

    if (!electricalFields.empty()) {
      detailRows.push_back(uiDivider());
      detailRows.push_back(fullLine("ELECTRICAL PARAMETERS", uiSecondaryText(), uiSurfaceBg()));
      for (const auto& field : electricalFields) {
        // The passive summary line above already prints the package value, and
        // "0603 (1608 Metric)" needs no label to be understood.
        if (passive && field.label == "Package: ") continue;
        detailRows.push_back(detailFieldLine(field, detailInnerWidth));
      }
    }

    // Detail fields carry no value when empty, so an absent one is dropped
    // rather than rendered as a dash.
    ftxui::Elements detailRowsExpanded;
    const auto detailField = [&](const string& label, const string& value, ftxui::Color valueColor) {
      if (trim(value).empty()) return;
      detailRowsExpanded.push_back(detailFieldLine({label + ": ", value, uiSecondaryText(), valueColor}, detailInnerWidth));
    };
    detailField("DigiKey", item->digikeyPartNumber, uiPrimaryText());
    detailField("SKU", item->sku, uiPrimaryText());
    detailField("Inventatory ID", item->inventatoryId, uiPrimaryText());
    detailField("Location", item->location, uiPrimaryText());
    detailField("Tags", renderTags(item->tags), uiPrimaryText());
    // Collapsible: the heading always states how many fields are hidden so the
    // block never looks like missing data.
    detailRows.push_back(uiDivider());
    detailRows.push_back(target(ftxui::hbox({
                                    styledText(stockDetailsExpanded_ ? " \xE2\x96\xBE DETAILS" : " \xE2\x96\xB8 DETAILS",
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
      detailRows.push_back(fullLine("LINKS", uiSecondaryText(), uiSurfaceBg()));
      for (auto& row : linkRows) detailRows.push_back(move(row));
    }

    if (!trim(item->notes).empty()) {
      detailRows.push_back(uiDivider());
      detailRows.push_back(fullLine("NOTES", uiSecondaryText(), uiSurfaceBg()));
      detailRows.push_back(ftxui::paragraphAlignLeft(item->notes) | ftxui::color(uiPrimaryText()));
    }
  } else {
    detailRows.push_back(fullLine("No item selected.", uiMutedColor(), uiPanelRightBg()));
  }
  return detailRows;
}

}  // namespace inventatory
