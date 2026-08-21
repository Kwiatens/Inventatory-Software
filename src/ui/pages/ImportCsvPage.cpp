// Inventatory - Hardware Inventory Management System
// CSV import review page rendering and keyboard handling.

#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

namespace inventatory {

using namespace std;

namespace {

ftxui::Element fixedCell(const string& text, int width, ftxui::Color color, bool rightAlign = false) {
  auto content = rightAlign ? ftxui::hbox({ftxui::filler(), styledText(ellipsize(text, static_cast<size_t>(max(0, width))), color)})
                            : ftxui::hbox({styledText(ellipsize(text, static_cast<size_t>(max(0, width))), color), ftxui::filler()});
  return content | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

string importStatusLabel(const CsvImportCandidate& candidate) {
  return candidate.hasConflict ? "MATCH" : "NEW";
}

ftxui::Color importStatusColor(const CsvImportCandidate& candidate) {
  return candidate.hasConflict ? uiWarnColor() : uiSuccessColor();
}

}  // namespace

ftxui::Element App::renderImportCsvUi() const {
  const auto* activeScreen = ftxui::ScreenInteractive::Active();
  const int screenWidth = activeScreen != nullptr ? activeScreen->dimx() : 120;
  const int screenHeight = activeScreen != nullptr ? activeScreen->dimy() : 40;

  if (importSyncPrompt_) {
    auto self = const_cast<App*>(this);
    ftxui::Elements promptRows;
    promptRows.push_back(fullLine("CSV review is complete.", uiTitleColor(), uiPanelRightBg()));
    promptRows.push_back(uiDivider());
    promptRows.push_back(fullLine("Sync accepted parts with DigiKey API?", uiAccentColor(), uiPanelRightBg()));
    promptRows.push_back(fullLine("Highly recommended: this fills datasheets, product links, categories, and parameters.",
                                  uiWarnColor(), uiPanelRightBg()));
    promptRows.push_back(uiDivider());
    promptRows.push_back(ftxui::hbox({
        target(styledText(" Sync with DigiKey ", uiInteractiveColor(), uiRaisedSurfaceBg()), "import.sync.yes",
               UiTargetKind::Button, [self] { self->finishCsvImport(true); }),
        ftxui::text("  "),
        target(styledText(" Finish without sync ", uiSecondaryText(), uiRaisedSurfaceBg()), "import.sync.no",
               UiTargetKind::Button, [self] { self->finishCsvImport(false); }),
    }));
    promptRows.push_back(uiDivider());
    promptRows.push_back(fullLine(importCompletionMessage(), uiInfoColor(), uiPanelRightBg()));

    auto prompt = panel("DigiKey metadata sync", move(promptRows), uiAccentColor(), uiAccentColor()) |
                  ftxui::bgcolor(uiPanelRightBg()) |
                  ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, max(64, min(screenWidth - 8, 96)));
    return ftxui::vbox({
        ftxui::filler(),
        ftxui::hbox({ftxui::filler(), prompt, ftxui::filler()}),
        ftxui::filler(),
    });
  }

  if (importCandidates_.empty()) {
    auto self = const_cast<App*>(this);
    auto choose = target(styledText(" Choose CSV file ", uiInteractiveColor(), uiRaisedSurfaceBg()),
                         "import.choose", UiTargetKind::Button, [self] { self->beginCsvImport(); });
    return ftxui::vbox({
        ftxui::filler(),
        ftxui::hbox({ftxui::filler(), styledText("IMPORT COMPONENTS", uiPrimaryText()) | ftxui::bold,
                     ftxui::filler()}),
        ftxui::hbox({ftxui::filler(), styledText("DigiKey order CSV  ·  KiCad BOM", uiInfoColor()),
                     ftxui::filler()}),
        ftxui::text(""),
        ftxui::hbox({ftxui::filler(), choose, ftxui::filler()}),
        ftxui::text(""),
        ftxui::hbox({ftxui::filler(), styledText("The format is detected automatically", uiMutedText()),
                     ftxui::filler()}),
        ftxui::filler(),
    });
  }

  const int detailOuterWidth = clamp(screenWidth / 3, 42, 58);
  const int listOuterWidth = max(42, screenWidth - detailOuterWidth - 1);
  const int listInnerWidth = max(20, listOuterWidth - 2);
  const int statusWidth = 7;
  const int qtyWidth = 8;
  const int partWidth = clamp(listInnerWidth / 3, 22, 38);
  const int manufacturerWidth = clamp(listInnerWidth / 5, 14, 24);
  const int skuWidth = max(14, listInnerWidth - statusWidth - qtyWidth - partWidth - manufacturerWidth - 10);

  ftxui::Elements listRows;
  listRows.push_back(ftxui::hbox({
      fixedCell("Status", statusWidth, uiMutedColor()),
      styledText(" | ", uiDimColor()),
      fixedCell("Part", partWidth, uiMutedColor()),
      styledText(" | ", uiDimColor()),
      fixedCell("Manufacturer", manufacturerWidth, uiMutedColor()),
      styledText(" | ", uiDimColor()),
      fixedCell("Mfr Part", skuWidth, uiMutedColor()),
      ftxui::filler(),
      fixedCell("Qty", qtyWidth, uiMutedColor(), true),
  }) | ftxui::bgcolor(uiPanelLeftBg()));

  if (importCandidates_.empty()) {
    listRows.push_back(fullLine("No import rows waiting for review.", uiMutedColor(), uiPanelLeftBg()));
  } else {
    const int visibleRows = max(8, screenHeight - 9);
    size_t start = 0;
    if (importSelection_ >= static_cast<size_t>(visibleRows)) {
      start = importSelection_ - static_cast<size_t>(visibleRows) + 1;
    }

    const auto end = min(importCandidates_.size(), start + static_cast<size_t>(visibleRows));
    for (size_t index = start; index < end; ++index) {
      const auto& candidate = importCandidates_[index];
      const bool selected = index == importSelection_;
      const auto bg = selected ? uiSelectionBg() : uiSurfaceBg();
      const auto fg = selected ? uiTitleColor() : uiMutedColor();
      auto row = ftxui::hbox({
          fixedCell(importStatusLabel(candidate), statusWidth, importStatusColor(candidate)),
          styledText(" | ", uiDimColor()),
          fixedCell(candidate.item.partName, partWidth, fg),
          styledText(" | ", uiDimColor()),
          fixedCell(candidate.item.manufacturer, manufacturerWidth, selected ? uiTitleColor() : uiLabelColor()),
          styledText(" | ", uiDimColor()),
          fixedCell(candidate.item.sku, skuWidth, selected ? uiTitleColor() : uiInfoColor()),
          ftxui::filler(),
          fixedCell(to_string(candidate.item.quantity), qtyWidth, uiSuccessColor(), true),
      }) | ftxui::bgcolor(bg);
      if (selected) {
        row = row | ftxui::select;
      }
      auto self = const_cast<App*>(this);
      listRows.push_back(target(row, "import.row." + to_string(index), UiTargetKind::Row, [self, index] {
        self->importSelection_ = index;
        self->dirty_ = true;
      }));
    }
  }

  ftxui::Elements detailRows;
  auto self = const_cast<App*>(this);
  if (const auto* candidate = currentImportCandidate()) {
    detailRows.push_back(ftxui::hbox({
        target(styledText(" Accept ", uiInteractiveColor(), uiRaisedSurfaceBg()), "import.accept", UiTargetKind::Button,
               [self] { self->acceptImportCandidate(); }),
        ftxui::text("  "),
        target(styledText(" Edit ", uiInteractiveColor(), uiRaisedSurfaceBg()), "import.edit", UiTargetKind::Button,
               [self] { self->beginEditImportCandidate(); }),
        ftxui::text("  "),
        target(styledText(" Skip ", uiSecondaryText(), uiRaisedSurfaceBg()), "import.skip", UiTargetKind::Button,
               [self] { self->skipImportCandidate(); }),
        ftxui::filler(),
    }));
    detailRows.push_back(ftxui::separator() | ftxui::color(uiDividerColor()));
    detailRows.push_back(fullLine(candidate->hasConflict ? "Conflict guidance" : "New item preview",
                                  candidate->hasConflict ? uiWarnColor() : uiAccentColor(), uiPanelRightBg()));
    if (candidate->hasConflict) {
      detailRows.push_back(fullLine("Matched field: " + candidate->matchedField, uiWarnColor(), uiPanelRightBg()));
      detailRows.push_back(fullLine("Existing: " + candidate->existingPartName, uiTitleColor(), uiPanelRightBg()));
      detailRows.push_back(fullLine("Existing qty: " + to_string(candidate->existingQuantity), uiMutedColor(), uiPanelRightBg()));
      detailRows.push_back(fullLine("Incoming qty: " + to_string(candidate->item.quantity), uiSuccessColor(), uiPanelRightBg()));
      detailRows.push_back(fullLine("After Enter: qty " + to_string(candidate->existingQuantity + candidate->item.quantity),
                                    uiAccentColor(), uiPanelRightBg()));
      detailRows.push_back(uiDivider());
    }

    for (const auto& field : detailCoreFields(candidate->item)) {
      detailRows.push_back(detailFieldLine(field, detailOuterWidth - 4));
    }
    detailRows.push_back(uiDivider());
    detailRows.push_back(fullLine("DigiKey: " + renderUrl(candidate->item.digikeyPartNumber), uiLinkColor(), uiPanelRightBg()));
    detailRows.push_back(fullLine("Product: " + renderUrl(candidate->item.productUrl), uiLinkColor(), uiPanelRightBg()));
    detailRows.push_back(fullLine("Source row: " + to_string(candidate->sourceRow), uiMutedColor(), uiPanelRightBg()));
    detailRows.push_back(uiDivider());
    detailRows.push_back(fullLine("Enter accept   e edit   Backspace skip", uiDimColor(), uiPanelRightBg()));
  } else {
    detailRows.push_back(fullLine("Review complete.", uiAccentColor(), uiPanelRightBg()));
  }

  listRows.insert(listRows.begin(), fullLine("CSV ROWS  " + to_string(importCandidates_.size()), uiSecondaryText(), uiSurfaceBg()));
  auto listPanel = ftxui::vbox(move(listRows)) | ftxui::yframe | ftxui::vscroll_indicator |
                   ftxui::bgcolor(uiSurfaceBg()) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, listOuterWidth);
  detailRows.insert(detailRows.begin(), fullLine("IMPORT DETAIL", uiSecondaryText(), uiSurfaceBg()));
  auto detailPanel = ftxui::vbox(move(detailRows)) | ftxui::bgcolor(uiSurfaceBg()) |
                     ftxui::size(ftxui::WIDTH, ftxui::EQUAL, detailOuterWidth);

  return ftxui::hbox({
      listPanel,
      ftxui::separator() | ftxui::color(uiDimColor()),
      detailPanel,
  });
}

void App::handleImportCsvKey(const KeyEvent& key) {
  if (importSyncPrompt_) {
    if (key.type == KeyType::Enter) {
      finishCsvImport(true);
      return;
    }
    if (key.type == KeyType::Escape) {
      finishCsvImport(false);
      return;
    }
    if (key.type == KeyType::Character) {
      const auto ch = tolower(static_cast<unsigned char>(key.ch));
      if (ch == 'y') {
        finishCsvImport(true);
      } else if (ch == 'n') {
        finishCsvImport(false);
      }
    }
    return;
  }

  if (key.type == KeyType::Character) {
    const auto ch = tolower(static_cast<unsigned char>(key.ch));
    switch (ch) {
      case 'j':
        moveImportSelection(1);
        break;
      case 'k':
        moveImportSelection(-1);
        break;
      case 'e':
        beginEditImportCandidate();
        break;
      case 'q':
        changePage(Page::Home);
        setMessage("CSV import cancelled", 3);
        break;
      default:
        break;
    }
    return;
  }

  if (key.type == KeyType::Up) {
    moveImportSelection(-1);
  } else if (key.type == KeyType::Down) {
    moveImportSelection(1);
  } else if (key.type == KeyType::PageUp) {
    moveImportSelection(-10);
  } else if (key.type == KeyType::PageDown) {
    moveImportSelection(10);
  } else if (key.type == KeyType::Home) {
    importSelection_ = 0;
    dirty_ = true;
  } else if (key.type == KeyType::End) {
    if (!importCandidates_.empty()) {
      importSelection_ = importCandidates_.size() - 1;
      dirty_ = true;
    }
  } else if (key.type == KeyType::Enter) {
    acceptImportCandidate();
  } else if (key.type == KeyType::Backspace) {
    skipImportCandidate();
  } else if (key.type == KeyType::Escape) {
    changePage(Page::Home);
    setMessage("CSV import cancelled", 3);
  }
}

}  // namespace inventatory
