// Inventatory - local manufacturer catalogue management screen.
#include "App.h"

#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace inventatory {

using namespace std;

namespace {
ftxui::Element settingLine(const string& label, const string& value, int width) {
  const int labelWidth = min(22, max(14, width / 3));
  return ftxui::hbox({
             styledText(" " + label, uiSecondaryText()) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, labelWidth),
             styledText(ellipsize(value, static_cast<size_t>(max(8, width - labelWidth - 2))), uiPrimaryText()),
             ftxui::filler(),
         }) |
         ftxui::bgcolor(uiSurfaceBg());
}

const ManufacturerSource* selectedSource(const size_t selection) {
  const auto& sources = manufacturerSources();
  return selection < sources.size() ? &sources[selection] : nullptr;
}

const CatalogueProfile* profileFor(const ManufacturerSource& source) {
  for (const auto& profile : catalogueProfiles()) if (profile.id == source.profileId) return &profile;
  return nullptr;
}

const array<string, 10> kManualMappingLabels = {"Part number (required)", "Base part (optional)",
    "Package (optional)", "Description (optional)", "Capacitance (F)", "Resistance (Ohm)",
    "Voltage rating (V)", "Current rating (A)", "Power (W)", "Tolerance (%)"};

const array<pair<string, string>, 6> kManualProperties = {{{"capacitance", "F"}, {"resistance", "Ohm"},
    {"voltage_rating", "V"}, {"current_rating", "A"}, {"power", "W"}, {"tolerance", "%"}}};

string manualColumnLabel(const CatalogueImportPreview& preview, size_t selection) {
  return selection == 0 ? "Not mapped" : preview.headers[selection - 1];
}

string importedAtLabel(time_t value) {
  if (value <= 0) return "date unavailable";
  tm local{};
  if (localtime_s(&local, &value) != 0) return "date unavailable";
  char buffer[32]{};
  return strftime(buffer, sizeof(buffer), "%d %b %Y %H:%M", &local) == 0 ? "date unavailable" : buffer;
}

string sourceCategoriesLabel(const ManufacturerSource& source) {
  string label;
  for (const auto& category : source.supportedCategories) {
    if (!label.empty()) label += " / ";
    label += category;
  }
  return label.empty() ? "Unspecified" : label;
}
}  // namespace

void App::beginCatalogueDownload() {
  const auto* source = selectedSource(catalogueSourceSelection_);
  if (!source) return;
  if (!catalogueDownloadSession_.start(*source)) {
    setMessage("Downloads folder is unavailable; choose the exported CSV or XLSX file manually", 6);
    return;
  }
  if (!openUrl(source->officialDownloadPage)) {
    catalogueDownloadSession_.cancel();
    setMessage("Could not open the official selector; choose the exported file manually", 6);
    return;
  }
  catalogueFlow_ = CatalogueFlow::WaitingForDownload;
  setMessage("Official selector opened. Waiting only for a new local download.", 6);
  dirty_ = true;
}

void App::chooseCatalogueFile() {
  filesystem::path selected;
  if (!openCatalogueFileDialog(selected)) return;
  catalogueDownloadSession_.cancel();
  catalogueManualProfile_ = {};
  catalogueSelectedPath_ = move(selected);
  const auto* source = selectedSource(catalogueSourceSelection_);
  cataloguePreview_ = catalogueDatabase_.previewFile(catalogueSelectedPath_, source ? profileFor(*source) : nullptr);
  if (!cataloguePreview_.valid()) {
    setMessage(cataloguePreview_.error, 7);
    return;
  }
  catalogueFlow_ = CatalogueFlow::Preview;
  setMessage("Catalogue selected. Review the source and press Enter to import.", 5);
  dirty_ = true;
}

void App::chooseManualCatalogueFile() {
  filesystem::path selected;
  if (!openCatalogueFileDialog(selected)) return;
  catalogueDownloadSession_.cancel();
  catalogueManualProfile_ = {};
  catalogueSelectedPath_ = move(selected);
  const auto* source = selectedSource(catalogueSourceSelection_);
  if (source) {
    if (const auto saved = catalogueDatabase_.localMapping(source->profileId)) {
      cataloguePreview_ = catalogueDatabase_.previewFile(catalogueSelectedPath_, &*saved);
      if (cataloguePreview_.valid() && find(cataloguePreview_.headers.begin(), cataloguePreview_.headers.end(), saved->mpnColumns.front()) != cataloguePreview_.headers.end()) {
        catalogueManualProfile_ = *saved;
        catalogueFlow_ = CatalogueFlow::Preview;
        setMessage("Saved local mapping applied. Review the import before committing it.", 6);
        dirty_ = true;
        return;
      }
    }
  }
  cataloguePreview_ = catalogueDatabase_.previewFile(catalogueSelectedPath_);
  if (!cataloguePreview_.valid()) {
    setMessage(cataloguePreview_.error, 7);
    return;
  }
  catalogueManualMappingStep_ = 0;
  catalogueManualCategorySelection_ = 0;
  catalogueManualColumns_.fill(0);
  catalogueManualColumnSelection_ = cataloguePreview_.headers.empty() ? 0 : 1;
  catalogueFlow_ = CatalogueFlow::ManualMapping;
  setMessage("Choose identity columns; other useful scalar columns remain local raw properties.", 7);
  dirty_ = true;
}

void App::beginCatalogueImport() {
  const auto* source = selectedSource(catalogueSourceSelection_);
  const auto* profile = !catalogueManualProfile_.id.empty() ? &catalogueManualProfile_ : source ? profileFor(*source) : nullptr;
  if (catalogueSelectedPath_.empty() || !profile) {
    setMessage("Select a manufacturer source and a CSV or XLSX file first", 4);
    return;
  }
  catalogueImportCancelled_.store(false);
  {
    lock_guard<mutex> lock(catalogueProgressMutex_);
    catalogueProgress_ = {0, 0, "Preparing import"};
  }
  const auto path = catalogueSelectedPath_;
  catalogueFlow_ = CatalogueFlow::Importing;
  catalogueImportFuture_ = async(launch::async, [this, path, profile] {
    CatalogueImportOptions options;
    options.cancel = &catalogueImportCancelled_;
    options.progress = [this](size_t completed, size_t total, const string& stage) {
      lock_guard<mutex> lock(catalogueProgressMutex_);
      catalogueProgress_ = {completed, total, stage};
    };
    return catalogueDatabase_.importFile(path, profile, options);
  });
  dirty_ = true;
}

void App::pollCatalogueImport() {
  if (catalogueFlow_ == CatalogueFlow::WaitingForDownload) {
    if (const auto detected = catalogueDownloadSession_.poll()) {
      catalogueSelectedPath_ = *detected;
      const auto* source = selectedSource(catalogueSourceSelection_);
      cataloguePreview_ = catalogueDatabase_.previewFile(catalogueSelectedPath_, source ? profileFor(*source) : nullptr);
      if (!cataloguePreview_.valid()) {
        catalogueFlow_ = CatalogueFlow::Sources;
        setMessage(cataloguePreview_.error, 7);
        dirty_ = true;
        return;
      }
      catalogueFlow_ = CatalogueFlow::Preview;
      setMessage("Catalogue detected. Review it before importing.", 5);
      dirty_ = true;
    } else if (!catalogueDownloadSession_.active() && catalogueDownloadSession_.timedOut()) {
      catalogueFlow_ = CatalogueFlow::Sources;
      setMessage("Download detection timed out; choose the exported CSV or XLSX file manually", 7);
      dirty_ = true;
    }
    return;
  }
  if (catalogueFlow_ != CatalogueFlow::Importing || !catalogueImportFuture_.valid() ||
      catalogueImportFuture_.wait_for(chrono::milliseconds(0)) != future_status::ready) return;
  catalogueImportResult_ = catalogueImportFuture_.get();
  catalogueFlow_ = CatalogueFlow::Results;
  if (!catalogueImportResult_.error.empty()) setMessage(catalogueImportResult_.error, 8);
  else if (catalogueImportResult_.cancelled) setMessage("Catalogue import cancelled; the previous snapshot is unchanged", 6);
  else if (catalogueImportResult_.duplicate) setMessage("This exact catalogue snapshot is already installed", 5);
  else setMessage("Catalogue import completed", 5);
  dirty_ = true;
}

void App::reEnrichInventoryFromCatalogue() {
  size_t changed = 0;
  for (auto& item : store_.items()) {
    if (applyCatalogueEnrichment(item, catalogueDatabase_.lookup(item.manufacturer, item.manufacturerPartNumber))) ++changed;
  }
  if (changed != 0 && !saveState()) return;
  setMessage(to_string(changed) + " inventory item" + (changed == 1 ? " was" : "s were") + " re-enriched", 5);
}

ftxui::Element App::renderCatalogueUi() const {
  auto self = const_cast<App*>(this);
  const auto& sources = manufacturerSources();
  const auto snapshots = catalogueDatabase_.snapshots();
  ftxui::Elements rows;
  rows.push_back(styledText("ELECTRICAL DATA SOURCES", uiPrimaryText()) | ftxui::bold);
  rows.push_back(styledText("Local-only manufacturer parametric catalogues. Inventatory never fetches or redistributes them.", uiMutedText()));
  rows.push_back(uiDivider());

  if (catalogueRemovalConfirmation_) {
    const auto* source = selectedSource(catalogueSourceSelection_);
    rows.push_back(styledText("REMOVE LOCAL CATALOGUE?", uiDangerColor()) | ftxui::bold);
    rows.push_back(styledText(source ? source->displayName : "Selected source", uiPrimaryText()));
    rows.push_back(styledText("Catalogue rows and their local indexes will be removed. Inventory quantities, locations, labels, notes, and manual fields are preserved.", uiMutedText()));
    rows.push_back(styledText("Enter remove   Esc keep catalogue", uiDangerColor()));
    return ftxui::vbox(move(rows)) | ftxui::flex;
  }

  if (catalogueFlow_ == CatalogueFlow::WaitingForDownload) {
    const auto* source = selectedSource(catalogueSourceSelection_);
    rows.push_back(styledText("WAITING FOR CATALOGUE", uiSecondaryText()) | ftxui::bold);
    rows.push_back(styledText(source ? source->displayName : "Selected manufacturer", uiTitleColor()));
    rows.push_back(styledText("Watching: " + catalogueDownloadSession_.watchedDirectory().string(), uiMutedText()));
    rows.push_back(styledText("Waiting for a newly downloaded CSV or XLSX file. Partial browser files are ignored.", uiMutedText()));
    rows.push_back(styledText("O open official page again   F choose file manually   C cancel", uiInteractiveColor()));
    return ftxui::vbox(move(rows)) | ftxui::flex;
  }
  if (catalogueFlow_ == CatalogueFlow::Preview) {
    const auto* source = selectedSource(catalogueSourceSelection_);
    const auto& preview = cataloguePreview_;
    rows.push_back(styledText("CATALOGUE READY TO IMPORT", uiSecondaryText()) | ftxui::bold);
    rows.push_back(settingLine("Manufacturer", source ? source->displayName : "Unknown", 70));
    rows.push_back(settingLine("File", catalogueSelectedPath_.filename().string(), 70));
    rows.push_back(settingLine("Format", preview.format, 70));
    rows.push_back(settingLine("Size", to_string(preview.fileSize / 1024) + " KB", 70));
    rows.push_back(settingLine("Profile", preview.profileId + " v" + preview.profileVersion, 70));
    if (!preview.sheetName.empty()) rows.push_back(settingLine("Worksheet", preview.sheetName + " · header row " + to_string(preview.headerRow + 1), 70));
    else rows.push_back(settingLine("CSV", string("delimiter ") + preview.delimiter + " · " + to_string(preview.rows) + " rows", 70));
    rows.push_back(styledText("Mapped: " + to_string(preview.mappedColumns.size()) + " properties · preserved unmapped: " + to_string(preview.unmappedColumns.size()), uiSuccessColor()));
    for (const auto& warning : preview.warnings) rows.push_back(styledText("Warning: " + warning, uiWarnColor()));
    if (!preview.sampleRows.empty()) {
      rows.push_back(styledText("FIRST ROWS", uiSecondaryText()) | ftxui::bold);
      for (size_t row = 0; row < min<size_t>(10, preview.sampleRows.size()); ++row) {
        string line;
        for (size_t column = 0; column < min<size_t>(3, preview.sampleRows[row].size()); ++column) {
          if (!line.empty()) line += " · ";
          line += ellipsize(preview.sampleRows[row][column], 24);
        }
        rows.push_back(styledText("  " + line, uiMutedText()));
      }
    }
    rows.push_back(styledText("The validated source is committed only after you press Import.", uiMutedText()));
    rows.push_back(styledText("Enter import   F choose another file   Esc cancel", uiInteractiveColor()));
    return ftxui::vbox(move(rows)) | ftxui::flex;
  }
  if (catalogueFlow_ == CatalogueFlow::ManualMapping) {
    rows.push_back(styledText("MAP THIS CATALOGUE", uiSecondaryText()) | ftxui::bold);
    const auto* source = selectedSource(catalogueSourceSelection_);
    rows.push_back(styledText("Manufacturer: " + string(source ? source->displayName : "Selected source"), uiPrimaryText()));
    const string category = source && catalogueManualCategorySelection_ < source->supportedCategories.size()
                                ? source->supportedCategories[catalogueManualCategorySelection_] : "Unspecified";
    rows.push_back(styledText("Category: " + category, uiMutedText()));
    rows.push_back(styledText("Map identity fields. All other useful scalar columns are retained locally for future mapping.", uiMutedText()));
    rows.push_back(uiDivider());
    for (size_t index = 0; index < kManualMappingLabels.size(); ++index) {
      const bool selected = index == catalogueManualMappingStep_;
      rows.push_back(styledText(string(selected ? "> " : "  ") + kManualMappingLabels[index] + ": " +
                                manualColumnLabel(cataloguePreview_, catalogueManualColumns_[index]),
                                selected ? uiTitleColor() : uiPrimaryText()));
    }
    rows.push_back(uiDivider());
    rows.push_back(styledText("Left/Right choose category   Up/Down choose column   Enter next field   Esc cancel", uiInteractiveColor()));
    return ftxui::vbox(move(rows)) | ftxui::flex;
  }
  if (catalogueFlow_ == CatalogueFlow::Importing) {
    CatalogueProgress progress;
    { lock_guard<mutex> lock(catalogueProgressMutex_); progress = catalogueProgress_; }
    rows.push_back(styledText("IMPORTING CATALOGUE", uiSecondaryText()) | ftxui::bold);
    rows.push_back(styledText(progress.stage.empty() ? "Working locally..." : progress.stage, uiTitleColor()));
    rows.push_back(styledText(progress.total ? to_string(progress.completed) + " / " + to_string(progress.total) + " rows"
                                           : to_string(progress.completed) + " rows processed", uiMutedText()));
    rows.push_back(styledText("Esc cancels safely; no partial snapshot becomes active.", uiMutedText()));
    return ftxui::vbox(move(rows)) | ftxui::flex;
  }
  if (catalogueFlow_ == CatalogueFlow::Results) {
    const auto& result = catalogueImportResult_;
    rows.push_back(styledText(result.error.empty() ? "CATALOGUE IMPORT RESULT" : "CATALOGUE IMPORT FAILED", result.error.empty() ? uiSuccessColor() : uiDangerColor()) | ftxui::bold);
    rows.push_back(settingLine("Parts", to_string(result.parts), 70));
    rows.push_back(settingLine("Aliases", to_string(result.aliases), 70));
    rows.push_back(settingLine("Properties", to_string(result.properties), 70));
    rows.push_back(settingLine("Preserved unmapped", to_string(result.unmappedProperties), 70));
    rows.push_back(settingLine("Warnings", to_string(result.warnings), 70));
    rows.push_back(settingLine("Rejected rows", to_string(result.rejected), 70));
    rows.push_back(styledText("W view warnings   R re-enrich inventory   Esc return to sources", uiInteractiveColor()));
    return ftxui::vbox(move(rows)) | ftxui::flex;
  }
  if (catalogueFlow_ == CatalogueFlow::Details) {
    const auto* source = selectedSource(catalogueSourceSelection_);
    rows.push_back(styledText("LOCAL CATALOGUE DETAILS", uiSecondaryText()) | ftxui::bold);
    rows.push_back(styledText(source ? source->displayName : "Selected manufacturer", uiTitleColor()));
    if (catalogueDetails_.empty()) rows.push_back(styledText("No retained snapshots for this source.", uiMutedText()));
    for (size_t index = 0; index < catalogueDetails_.size(); ++index) {
      const auto& snapshot = catalogueDetails_[index];
      rows.push_back(styledText(string(index == 0 ? "Current: " : "History: ") + snapshot.filename + " · " +
                                importedAtLabel(snapshot.importedAt), index == 0 ? uiPrimaryText() : uiMutedText()));
      rows.push_back(styledText("  profile v" + snapshot.profileVersion + " · " + to_string(snapshot.parts) +
                                " parts · " + to_string(snapshot.aliases) + " aliases · " +
                                to_string(snapshot.properties) + " properties · " + to_string(snapshot.warnings) + " warnings", uiMutedText()));
    }
    rows.push_back(ftxui::filler());
    rows.push_back(styledText("Esc return to sources", uiInteractiveColor()));
    return ftxui::vbox(move(rows)) | ftxui::flex;
  }
  if (catalogueFlow_ == CatalogueFlow::Warnings) {
    rows.push_back(styledText("CATALOGUE IMPORT WARNINGS", uiWarnColor()) | ftxui::bold);
    if (catalogueWarnings_.empty()) rows.push_back(styledText("This snapshot has no import warnings.", uiSuccessColor()));
    for (const auto& warning : catalogueWarnings_) rows.push_back(styledText("Row " + to_string(warning.row) + ": " + warning.message, uiWarnColor()));
    rows.push_back(ftxui::filler());
    rows.push_back(styledText("Esc return to sources", uiInteractiveColor()));
    return ftxui::vbox(move(rows)) | ftxui::flex;
  }

  string lastGroup;
  for (size_t index = 0; index < sources.size(); ++index) {
    const auto& source = sources[index];
    const string group = index < 4 ? "PASSIVES" : index == 4 ? "ACTIVE COMPONENTS" : "INTEGRATED CIRCUITS";
    if (group != lastGroup) { rows.push_back(styledText(group, uiSecondaryText()) | ftxui::bold); lastGroup = group; }
    const auto installed = find_if(snapshots.begin(), snapshots.end(), [&](const CatalogueImportStats& snapshot) { return snapshot.profileId == source.profileId; });
    const bool isInstalled = installed != snapshots.end();
    string status = isInstalled ? "Installed · " + to_string(installed->parts) + " parts" : "Not installed";
    const bool selected = index == catalogueSourceSelection_;
    auto line = ftxui::hbox({styledText(" " + source.displayName, selected ? uiTitleColor() : uiPrimaryText()) | ftxui::bold,
                             ftxui::filler(), styledText(status + "  ", isInstalled ? uiSuccessColor() : uiMutedColor())});
    rows.push_back(target(line, "catalogue.source." + source.id, UiTargetKind::Row, [self, index] { self->catalogueSourceSelection_ = index; self->dirty_ = true; }));
    if (selected) rows.push_back(styledText("   Categories: " + sourceCategoriesLabel(source), uiMutedText()));
    if (selected && isInstalled) {
      const auto snapshotCount = count_if(snapshots.begin(), snapshots.end(), [&](const CatalogueImportStats& snapshot) { return snapshot.profileId == source.profileId; });
      rows.push_back(styledText("   Last import: " + importedAtLabel(installed->importedAt) + " · " + installed->filename + " · profile v" + installed->profileVersion +
                                " · " + to_string(installed->properties) + " properties · " +
                                to_string(installed->warnings) + " warnings", uiMutedText()));
      rows.push_back(styledText("   Local snapshot history: " + to_string(snapshotCount) + " retained import" + (snapshotCount == 1 ? "" : "s"), uiMutedText()));
      rows.push_back(styledText("   D view local catalogue details and retained snapshot history", uiInteractiveColor()));
    }
    rows.push_back(styledText("   " + source.supportedCategories.front() + " · " + (isInstalled ? "G update  F different file  W warnings  R re-enrich  X remove" : "G get catalogue  F choose file"), uiMutedText()));
  }
  rows.push_back(ftxui::filler());
  rows.push_back(styledText("↑↓ select   G official selector   F file   M map unknown   W warnings   R re-enrich   X remove", uiInteractiveColor()));
  return ftxui::vbox(move(rows)) | ftxui::flex;
}

void App::handleCatalogueKey(const KeyEvent& key) {
  if (catalogueRemovalConfirmation_) {
    if (key.type == KeyType::Enter) {
      const auto* source = selectedSource(catalogueSourceSelection_);
      if (source && catalogueDatabase_.removeSource(source->id)) {
        catalogueRemovalConfirmation_ = false;
        reEnrichInventoryFromCatalogue();
        setMessage(source->displayName + " catalogue removed; inventory remains intact", 6);
      } else {
        catalogueRemovalConfirmation_ = false;
        setMessage("Could not remove the local catalogue", 5);
      }
      dirty_ = true;
    } else if (key.type == KeyType::Escape) {
      catalogueRemovalConfirmation_ = false;
      dirty_ = true;
    }
    return;
  }
  if (catalogueFlow_ == CatalogueFlow::WaitingForDownload) {
    if (key.type == KeyType::Character && (key.ch == 'c' || key.ch == 'C')) { catalogueDownloadSession_.cancel(); catalogueFlow_ = CatalogueFlow::Sources; dirty_ = true; }
    else if (key.type == KeyType::Character && (key.ch == 'f' || key.ch == 'F')) chooseCatalogueFile();
    else if (key.type == KeyType::Character && (key.ch == 'o' || key.ch == 'O')) beginCatalogueDownload();
    return;
  }
  if (catalogueFlow_ == CatalogueFlow::Preview) {
    if (key.type == KeyType::Enter) beginCatalogueImport();
    else if (key.type == KeyType::Escape) { catalogueSelectedPath_.clear(); catalogueFlow_ = CatalogueFlow::Sources; dirty_ = true; }
    else if (key.type == KeyType::Character && (key.ch == 'f' || key.ch == 'F')) chooseCatalogueFile();
    return;
  }
  if (catalogueFlow_ == CatalogueFlow::ManualMapping) {
    const size_t optionCount = cataloguePreview_.headers.size() + 1;
    const auto* source = selectedSource(catalogueSourceSelection_);
    if (key.type == KeyType::Left && source && catalogueManualCategorySelection_ > 0) --catalogueManualCategorySelection_;
    else if (key.type == KeyType::Right && source && catalogueManualCategorySelection_ + 1 < source->supportedCategories.size()) ++catalogueManualCategorySelection_;
    else if (key.type == KeyType::Up && catalogueManualColumnSelection_ > 0) --catalogueManualColumnSelection_;
    else if (key.type == KeyType::Down && catalogueManualColumnSelection_ + 1 < optionCount) ++catalogueManualColumnSelection_;
    else if (key.type == KeyType::Escape) { catalogueSelectedPath_.clear(); catalogueFlow_ = CatalogueFlow::Sources; dirty_ = true; }
    else if (key.type == KeyType::Enter) {
      if (catalogueManualMappingStep_ == 0 && catalogueManualColumnSelection_ == 0) {
        setMessage("A part-number column is required for exact catalogue import", 5);
        return;
      }
      catalogueManualColumns_[catalogueManualMappingStep_] = catalogueManualColumnSelection_;
      if (++catalogueManualMappingStep_ < kManualMappingLabels.size()) {
        catalogueManualColumnSelection_ = catalogueManualColumns_[catalogueManualMappingStep_];
      } else {
        if (!source) return;
        const auto column = [&](size_t index) -> vector<string> {
          const auto selected = catalogueManualColumns_[index];
          return selected == 0 ? vector<string>{} : vector<string>{cataloguePreview_.headers[selected - 1]};
        };
        catalogueManualProfile_ = {source->profileId, "manual-v1", source->manufacturer,
                                   source->supportedCategories.empty() ? "Unspecified" : source->supportedCategories[min(catalogueManualCategorySelection_, source->supportedCategories.size() - 1)],
                                   "", {}, column(0), column(1), {}, column(2), {}, column(3), {}, {}, false};
        for (size_t index = 0; index < kManualProperties.size(); ++index) {
          const auto selected = catalogueManualColumns_[index + 4];
          if (selected != 0) catalogueManualProfile_.properties.push_back(
              {{cataloguePreview_.headers[selected - 1]}, kManualProperties[index].first, kManualProperties[index].second, ""});
        }
        cataloguePreview_ = catalogueDatabase_.previewFile(catalogueSelectedPath_, &catalogueManualProfile_);
        if (!cataloguePreview_.valid()) { setMessage(cataloguePreview_.error, 7); catalogueFlow_ = CatalogueFlow::Sources; return; }
        const bool saved = catalogueDatabase_.saveLocalMapping(catalogueManualProfile_);
        catalogueFlow_ = CatalogueFlow::Preview;
        setMessage(saved ? "Manual mapping saved locally. Review it before importing."
                         : "Manual mapping is ready, but could not be saved locally.", saved ? 5 : 7);
      }
      dirty_ = true;
    }
    return;
  }
  if (catalogueFlow_ == CatalogueFlow::Importing) { if (key.type == KeyType::Escape) catalogueImportCancelled_.store(true); return; }
  if (catalogueFlow_ == CatalogueFlow::Results) { if (key.type == KeyType::Escape) { catalogueFlow_ = CatalogueFlow::Sources; dirty_ = true; } else if (key.type == KeyType::Character && (key.ch == 'r' || key.ch == 'R')) reEnrichInventoryFromCatalogue(); else if (key.type == KeyType::Character && (key.ch == 'w' || key.ch == 'W')) { catalogueWarnings_ = catalogueDatabase_.warningsForSnapshot(catalogueImportResult_.snapshotId); catalogueFlow_ = CatalogueFlow::Warnings; dirty_ = true; } return; }
  if (catalogueFlow_ == CatalogueFlow::Details || catalogueFlow_ == CatalogueFlow::Warnings) { if (key.type == KeyType::Escape) { catalogueFlow_ = CatalogueFlow::Sources; dirty_ = true; } return; }
  if (key.type == KeyType::Up && catalogueSourceSelection_ > 0) --catalogueSourceSelection_;
  else if (key.type == KeyType::Down && catalogueSourceSelection_ + 1 < manufacturerSources().size()) ++catalogueSourceSelection_;
  else if (key.type == KeyType::Enter || (key.type == KeyType::Character && (key.ch == 'g' || key.ch == 'G'))) beginCatalogueDownload();
  else if (key.type == KeyType::Character && (key.ch == 'f' || key.ch == 'F')) chooseCatalogueFile();
  else if (key.type == KeyType::Character && (key.ch == 'm' || key.ch == 'M')) chooseManualCatalogueFile();
  else if (key.type == KeyType::Character && (key.ch == 'r' || key.ch == 'R')) reEnrichInventoryFromCatalogue();
  else if (key.type == KeyType::Character && (key.ch == 'w' || key.ch == 'W')) {
    const auto* source = selectedSource(catalogueSourceSelection_); const auto snapshots = catalogueDatabase_.snapshots();
    const auto snapshot = source ? find_if(snapshots.begin(), snapshots.end(), [&](const CatalogueImportStats& value) { return value.profileId == source->profileId; }) : snapshots.end();
    if (snapshot == snapshots.end()) setMessage("No installed catalogue exists for this source", 4); else { catalogueWarnings_ = catalogueDatabase_.warningsForSnapshot(snapshot->snapshotId); catalogueFlow_ = CatalogueFlow::Warnings; }
  }
  else if (key.type == KeyType::Character && (key.ch == 'd' || key.ch == 'D')) {
    const auto* source = selectedSource(catalogueSourceSelection_);
    const auto snapshots = catalogueDatabase_.snapshots();
    catalogueDetails_.clear();
    if (source) copy_if(snapshots.begin(), snapshots.end(), back_inserter(catalogueDetails_), [&](const CatalogueImportStats& value) { return value.profileId == source->profileId; });
    if (catalogueDetails_.empty()) setMessage("No installed catalogue exists for this source", 4); else catalogueFlow_ = CatalogueFlow::Details;
  }
  else if (key.type == KeyType::Character && (key.ch == 'x' || key.ch == 'X')) {
    const auto* source = selectedSource(catalogueSourceSelection_);
    const auto snapshots = catalogueDatabase_.snapshots();
    if (source && any_of(snapshots.begin(), snapshots.end(), [&](const CatalogueImportStats& snapshot) { return snapshot.profileId == source->profileId; })) {
      catalogueRemovalConfirmation_ = true;
    } else {
      setMessage("No installed catalogue exists for this source", 4);
    }
  }
  else if (key.type == KeyType::Escape) changePage(Page::Home);
  dirty_ = true;
}

}  // namespace inventatory
