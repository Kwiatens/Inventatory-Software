// Inventatory - in-app GitHub release update workflow.

#include "App.h"

#include "core/storage/AtomicFile.h"
#include "ui/shared/AppUiShared.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <future>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace inventatory {

using namespace std;

namespace {

constexpr size_t kMaximumMarkerBytes = 4096U;
constexpr size_t kMaximumNotesBytes = 64U * 1024U;

bool readBoundedFile(const filesystem::path& path, size_t maximum, string& contents) {
  contents.clear();
  ifstream input(path, ios::binary);
  if (!input) return false;
  contents.assign(istreambuf_iterator<char>(input), istreambuf_iterator<char>());
  return contents.size() <= maximum;
}

string markerValue(const string& marker, const string& key) {
  istringstream input(marker);
  string line;
  while (getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto separator = line.find('=');
    if (separator != string::npos && line.substr(0, separator) == key) return line.substr(separator + 1);
  }
  return {};
}

string sanitizeMarkerValue(string value, size_t maximum) {
  value.erase(remove_if(value.begin(), value.end(), [](char ch) { return ch == '\r' || ch == '\n'; }), value.end());
  if (value.size() > maximum) value.resize(maximum);
  return value;
}

string formatBytes(uint64_t bytes) {
  static constexpr const char* units[] = {"B", "KB", "MB", "GB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1U < size(units)) {
    value /= 1024.0;
    ++unit;
  }
  ostringstream output;
  output << fixed << setprecision(unit == 0 ? 0 : value < 10.0 ? 1 : 0) << value << ' ' << units[unit];
  return output.str();
}

string formatEta(double seconds) {
  if (!(seconds > 0.0) || !isfinite(seconds)) return "calculating ETA";
  const auto rounded = static_cast<uint64_t>(max(1.0, ceil(seconds)));
  const auto minutes = rounded / 60U;
  const auto remaining = rounded % 60U;
  if (minutes == 0) return to_string(remaining) + "s remaining";
  return to_string(minutes) + "m " + to_string(remaining) + "s remaining";
}

string previewNotes(const string& notes) {
  if (notes.empty()) return "No release notes were provided for this release.";
  constexpr size_t kPreviewCharacters = 1800U;
  if (notes.size() <= kPreviewCharacters) return notes;
  auto prefix = notes.substr(0, kPreviewCharacters);
  while (!prefix.empty() && (static_cast<unsigned char>(prefix.back()) & 0xC0U) == 0x80U) prefix.pop_back();
  return prefix + "\n\n… release notes continue after the update";
}

vector<string> wrapUpdateNotes(const string& notes, int width) {
  vector<string> lines;
  istringstream input(notes);
  string sourceLine;
  while (getline(input, sourceLine)) {
    if (!sourceLine.empty() && sourceLine.back() == '\r') sourceLine.pop_back();
    if (sourceLine.empty()) {
      lines.push_back({});
      continue;
    }
    auto wrapped = wrapText(sourceLine, width);
    lines.insert(lines.end(), wrapped.begin(), wrapped.end());
  }
  if (lines.empty()) lines.push_back({});
  return lines;
}

filesystem::path updateDownloadDirectory() {
  const auto process = static_cast<unsigned long long>(GetCurrentProcessId());
  const auto tick = static_cast<unsigned long long>(GetTickCount64());
  return filesystem::temp_directory_path() / ("Inventatory-update-" + to_string(process) + "-" + to_string(tick));
}

void removeUpdateDownloadDirectory(const filesystem::path& directory) {
  if (directory.empty()) return;
  error_code ignored;
  filesystem::remove_all(directory, ignored);
}

}  // namespace

void App::loadUpdateCompletionMarker() {
  updateCompletionPending_ = false;
  updateCompletionVersion_.clear();
  updateCompletionNotes_.clear();
  string marker;
  if (!readBoundedFile(updateMarkerPath_, kMaximumMarkerBytes, marker)) return;
  const auto state = markerValue(marker, "state");
  if (state != "complete" && state != "failed" && state != "pending") return;
  updateCompletionVersion_ = sanitizeMarkerValue(markerValue(marker, "version"), 128U);
  if (!readBoundedFile(updateNotesPath_, kMaximumNotesBytes, updateCompletionNotes_)) {
    updateCompletionNotes_ = "No release notes are available for this update.";
  }
  if (state == "pending") {
    updateError_ = "The update did not finish. Your previous installation is still active.";
    updateStep_ = UpdateWizardStep::Failed;
  } else if (state == "failed") {
    updateError_ = sanitizeMarkerValue(markerValue(marker, "error"), 512U);
    if (updateError_.empty()) updateError_ = "The update could not be completed. Your previous installation is still active.";
    updateStep_ = UpdateWizardStep::Failed;
  } else {
    updateStep_ = UpdateWizardStep::Complete;
  }
  updateCompletionPending_ = true;
  updateNotesScroll_ = 0;
}

bool App::writeUpdateMarker(const string& state, const string& version, const string& error) {
  const auto safeState = sanitizeMarkerValue(state, 32U);
  const auto safeVersion = sanitizeMarkerValue(version, 128U);
  const auto safeError = sanitizeMarkerValue(error, 512U);
  const string contents = "schema_version=1\nstate=" + safeState + "\nversion=" + safeVersion +
                          "\nerror=" + safeError + "\n";
  string writeError;
  if (!writeFileAtomically(updateMarkerPath_, contents, &writeError)) {
    updateError_ = writeError.empty() ? "Could not record the update state" : writeError;
    return false;
  }
  return true;
}

void App::beginSoftwareUpdate() {
  if (updateOperationFuture_.valid() || updateStep_ == UpdateWizardStep::HandingOff) return;
  if (!isVersionNewer(settings_.latestAvailableVersion, softwareVersion())) {
    beginUpdateChecks();
    setMessage("Checking for the latest Inventatory release", 4);
    return;
  }
  updateCompletionPending_ = false;
  updateError_.clear();
  updatePackage_.reset();
  updateNotesScroll_ = 0;
  inputMode_ = InputMode::None;
  focusedTarget_ = -1;
  page_ = Page::Update;
  if (settingsDirty_) {
    updateStep_ = UpdateWizardStep::SavePrompt;
  } else {
    beginUpdatePreparation();
  }
  dirty_ = true;
}

void App::beginUpdatePreparation() {
  if (updateOperationFuture_.valid()) return;
  updateStep_ = UpdateWizardStep::Preparing;
  updateError_.clear();
  dirty_ = true;
  const auto installedVersion = softwareVersion();
  updateOperationFuture_ = async(launch::async, [installedVersion] {
    UpdateOperationResult result;
    result.package.release = checkLatestRelease(installedVersion);
    if (!result.package.release.completed) {
      result.error = "Inventatory could not read the latest GitHub release.";
    } else if (!result.package.release.updateAvailable) {
      result.error = "No newer Inventatory release is available.";
    } else {
      result.success = true;
    }
    return result;
  });
}

void App::beginUpdateDownload() {
  if (!updatePackage_.has_value() || updateOperationFuture_.valid()) return;
  updateStep_ = UpdateWizardStep::Downloading;
  updateError_.clear();
  updateNotesScroll_ = 0;
  auto state = make_shared<UpdateDownloadState>();
  updateDownloadState_ = state;
  const auto package = *updatePackage_;
  updateOperationFuture_ = async(launch::async, [package, state]() mutable {
    UpdateOperationResult result;
    result.package = package;
    error_code filesystemError;
    filesystem::create_directories(package.downloadDirectory, filesystemError);
    if (filesystemError) {
      result.error = "Could not create a temporary update folder.";
      return result;
    }
    const auto progress = [state](const string& asset, uint64_t downloaded, uint64_t total) {
      if (state->cancelRequested.load()) return false;
      lock_guard<mutex> lock(state->mutex);
      if (state->startedAt.time_since_epoch().count() == 0) state->startedAt = chrono::steady_clock::now();
      const auto elapsed = chrono::duration<double>(chrono::steady_clock::now() - state->startedAt).count();
      state->assetName = asset;
      state->downloadedBytes = downloaded;
      state->totalBytes = total;
      if (elapsed > 0.05) state->bytesPerSecond = static_cast<double>(downloaded) / elapsed;
      return !state->cancelRequested.load();
    };
    const auto download = [&](const string& asset, const filesystem::path& destination) {
      {
        lock_guard<mutex> lock(state->mutex);
        state->assetName = asset;
        state->downloadedBytes = 0;
        state->totalBytes = 0;
        state->bytesPerSecond = 0.0;
        state->startedAt = chrono::steady_clock::now();
        state->verifying = false;
      }
      const auto url = buildReleaseAssetUrl(Inventatory_RELEASE_REPOSITORY, package.release.latestVersion, asset);
      return !url.empty() && downloadReleaseAsset(url, destination, progress, result.error);
    };
    if (!download("Inventatory-win-x64.zip", result.package.archivePath) ||
        !download("SHA256SUMS.txt", result.package.checksumsPath) ||
        !download("Install-Inventatory.ps1", result.package.installerPath)) {
      result.cancelled = state->cancelRequested.load();
      if (result.cancelled) result.error = "Update download cancelled.";
      return result;
    }
    if (state->cancelRequested.load()) {
      result.cancelled = true;
      result.error = "Update download cancelled.";
      return result;
    }
    result.success = true;
    return result;
  });
  dirty_ = true;
}

void App::beginUpdateVerification() {
  if (!updatePackage_.has_value() || updateOperationFuture_.valid()) return;
  updateStep_ = UpdateWizardStep::Verifying;
  updateError_.clear();
  const auto package = *updatePackage_;
  const auto state = updateDownloadState_;
  if (state != nullptr) {
    lock_guard<mutex> lock(state->mutex);
    state->assetName = "Verifying checksums";
    state->downloadedBytes = 0;
    state->totalBytes = 0;
    state->bytesPerSecond = 0.0;
    state->startedAt = chrono::steady_clock::now();
    state->verifying = true;
  }
  updateOperationFuture_ = async(launch::async, [package, state]() {
    UpdateOperationResult result;
    result.package = package;
    if (state != nullptr && state->cancelRequested.load()) {
      result.cancelled = true;
      result.error = "Update verification cancelled.";
      return result;
    }
    string checksums;
    if (!readBoundedFile(result.package.checksumsPath, 1U * 1024U * 1024U, checksums)) {
      result.error = "The checksum manifest is missing or too large.";
      return result;
    }
    string archiveHash;
    string installerHash;
    if (!parseSha256Checksum(checksums, "Inventatory-win-x64.zip", archiveHash) ||
        !parseSha256Checksum(checksums, "Install-Inventatory.ps1", installerHash)) {
      result.error = "The release checksum manifest does not contain the update assets.";
      return result;
    }
    if (!verifyReleaseFileSha256(result.package.archivePath, archiveHash, result.error)) return result;
    if (state != nullptr && state->cancelRequested.load()) {
      result.cancelled = true;
      result.error = "Update verification cancelled.";
      return result;
    }
    if (!verifyReleaseFileSha256(result.package.installerPath, installerHash, result.error)) return result;
    if (state != nullptr && state->cancelRequested.load()) {
      result.cancelled = true;
      result.error = "Update verification cancelled.";
      return result;
    }
    result.success = true;
    return result;
  });
  dirty_ = true;
}

void App::processSoftwareUpdate() {
  if (!updateOperationFuture_.valid()) return;
  if (updateOperationFuture_.wait_for(chrono::milliseconds(0)) != future_status::ready) return;
  auto result = updateOperationFuture_.get();
  if (updateStep_ == UpdateWizardStep::Preparing) {
    if (!result.success) {
      if (result.error == "No newer Inventatory release is available.") {
        page_ = Page::Settings;
        updateStep_ = UpdateWizardStep::Preparing;
        setMessage(result.error, 4);
      } else {
        updateStep_ = UpdateWizardStep::Failed;
        updateError_ = result.error;
      }
    } else {
      result.package.downloadDirectory = updateDownloadDirectory();
      result.package.archivePath = result.package.downloadDirectory / "Inventatory-win-x64.zip";
      result.package.checksumsPath = result.package.downloadDirectory / "SHA256SUMS.txt";
      result.package.installerPath = result.package.downloadDirectory / "Install-Inventatory.ps1";
      updatePackage_ = move(result.package);
      updateStep_ = UpdateWizardStep::Preview;
      updateNotesScroll_ = 0;
    }
  } else if (updateStep_ == UpdateWizardStep::Downloading) {
    if (!result.success) {
      if (result.cancelled) {
        updateStep_ = UpdateWizardStep::Preview;
        updateError_ = "Update download cancelled.";
      } else {
        updateStep_ = UpdateWizardStep::Failed;
        updateError_ = result.error.empty() ? "The update package could not be downloaded." : result.error;
      }
      removeUpdateDownloadDirectory(result.package.downloadDirectory);
      updateDownloadState_.reset();
    } else {
      updatePackage_ = move(result.package);
      updateStep_ = UpdateWizardStep::Verifying;
      beginUpdateVerification();
    }
  } else if (updateStep_ == UpdateWizardStep::Verifying) {
    if (!result.success) {
      if (result.cancelled) {
        updateStep_ = UpdateWizardStep::Preview;
        updateError_ = "Update verification cancelled.";
      } else {
        updateStep_ = UpdateWizardStep::Failed;
        updateError_ = result.error.empty() ? "The update package failed checksum verification." : result.error;
      }
      removeUpdateDownloadDirectory(result.package.downloadDirectory);
      updateDownloadState_.reset();
    } else {
      updatePackage_ = move(result.package);
      updateStep_ = UpdateWizardStep::HandingOff;
      updateDownloadState_.reset();
      prepareUpdateHandoff();
    }
  }
  dirty_ = true;
}

bool App::prepareUpdateHandoff() {
  if (!updatePackage_.has_value()) {
    updateStep_ = UpdateWizardStep::Failed;
    updateError_ = "The verified update package is unavailable.";
    return false;
  }
  if (settingsDirty_) {
    updateStep_ = UpdateWizardStep::SavePrompt;
    updateError_ = "Save the pending settings before starting the update.";
    return false;
  }
  if (importSyncRunning_ || hasPendingPersistence()) {
    updateStep_ = UpdateWizardStep::Failed;
    updateError_ = "Inventatory has pending work that must be saved before updating.";
    return false;
  }
  const bool serviceWasRunning = server_.running();
  mdnsService_.stop();
  server_.stop();
  stopWorkspaceBoundWork();
  if (!saveState()) {
    if (serviceWasRunning) restartDeviceService();
    updateStep_ = UpdateWizardStep::Failed;
    updateError_ = persistenceError_.empty() ? "Inventatory could not save before updating." : persistenceError_;
    return false;
  }
  string notesError;
  if (!writeFileAtomically(updateNotesPath_, updatePackage_->release.releaseNotes, &notesError) ||
      !writeUpdateMarker("pending", updatePackage_->release.latestVersion)) {
    if (serviceWasRunning) restartDeviceService();
    updateStep_ = UpdateWizardStep::Failed;
    updateError_ = notesError.empty() ? updateError_ : notesError;
    return false;
  }
  string launchError;
  if (!launchUpdateInstaller(updatePackage_->installerPath, updatePackage_->archivePath,
                             updatePackage_->checksumsPath, updateMarkerPath_, updateNotesPath_,
                             updatePackage_->release.latestVersion, launchError)) {
    writeUpdateMarker("failed", updatePackage_->release.latestVersion, launchError);
    if (serviceWasRunning) restartDeviceService();
    updateStep_ = UpdateWizardStep::Failed;
    updateError_ = launchError;
    return false;
  }
  updateInstallerLaunched_ = true;
  running_ = false;
  return true;
}

void App::cancelSoftwareUpdate() {
  if ((updateStep_ == UpdateWizardStep::Downloading || updateStep_ == UpdateWizardStep::Verifying) &&
      updateDownloadState_ != nullptr) {
    updateDownloadState_->cancelRequested.store(true);
    return;
  }
  if (updateOperationFuture_.valid()) return;
  updatePackage_.reset();
  updateError_.clear();
  updateStep_ = UpdateWizardStep::Preparing;
  page_ = Page::Settings;
  focusedTarget_ = -1;
  dirty_ = true;
}

void App::retrySoftwareUpdate() {
  if (updateOperationFuture_.valid()) return;
  updateCompletionPending_ = false;
  updateError_.clear();
  updatePackage_.reset();
  updateStep_ = UpdateWizardStep::Preparing;
  updateNotesScroll_ = 0;
  page_ = Page::Update;
  beginUpdatePreparation();
  dirty_ = true;
}

void App::dismissUpdateResult() {
  error_code ignored;
  filesystem::remove(updateMarkerPath_, ignored);
  filesystem::remove(updateNotesPath_, ignored);
  updateCompletionPending_ = false;
  updateCompletionVersion_.clear();
  updateCompletionNotes_.clear();
  updateError_.clear();
  updateStep_ = UpdateWizardStep::Preparing;
  updateNotesScroll_ = 0;
  page_ = Page::Home;
  focusedTarget_ = -1;
  dirty_ = true;
}

void App::handleUpdateKey(const KeyEvent& key) {
  const char ch = key.type == KeyType::Character ? static_cast<char>(tolower(static_cast<unsigned char>(key.ch))) : '\0';
  if (key.type == KeyType::Tab) {
    moveUiFocus(1);
    return;
  }
  if (key.type == KeyType::TabReverse) {
    moveUiFocus(-1);
    return;
  }
  if (key.type == KeyType::Enter && activateFocusedTarget()) return;
  if (updateStep_ == UpdateWizardStep::Downloading || updateStep_ == UpdateWizardStep::Verifying) {
    if (key.type == KeyType::Escape || ch == 'c') cancelSoftwareUpdate();
    return;
  }
  if (updateStep_ == UpdateWizardStep::Complete) {
    if (key.type == KeyType::Escape || key.type == KeyType::Enter) dismissUpdateResult();
    else if (key.type == KeyType::Up) updateNotesScroll_ = updateNotesScroll_ > 0 ? updateNotesScroll_ - 1U : 0U;
    else if (key.type == KeyType::Down || key.type == KeyType::PageDown) ++updateNotesScroll_;
    dirty_ = true;
    return;
  }
  if (updateStep_ == UpdateWizardStep::Failed) {
    if (ch == 'r') retrySoftwareUpdate();
    else if (key.type == KeyType::Escape || key.type == KeyType::Enter) {
      if (updateCompletionPending_) dismissUpdateResult();
      else cancelSoftwareUpdate();
    }
    return;
  }
  if (updateStep_ == UpdateWizardStep::SavePrompt) {
    if (ch == 's' || key.type == KeyType::Enter) {
      if (saveSettingsDraft()) beginUpdatePreparation();
    } else if (key.type == KeyType::Escape || ch == 'c') {
      cancelSoftwareUpdate();
    }
    return;
  }
  if (updateStep_ == UpdateWizardStep::Preview) {
    if (ch == 's' || key.type == KeyType::Enter) beginUpdateDownload();
    else if (key.type == KeyType::Escape || ch == 'c') cancelSoftwareUpdate();
    else if (key.type == KeyType::Up) updateNotesScroll_ = updateNotesScroll_ > 0 ? updateNotesScroll_ - 1U : 0U;
    else if (key.type == KeyType::Down || key.type == KeyType::PageDown) ++updateNotesScroll_;
    dirty_ = true;
  }
}

ftxui::Element App::renderUpdateContent() const {
  auto self = const_cast<App*>(this);
  const auto* active = ftxui::ScreenInteractive::Active();
  const int width = max(60, (active != nullptr ? active->dimx() : 120) - 12);
  const int notesHeight = max(5, min(10, (active != nullptr ? active->dimy() : 30) - 18));
  ftxui::Elements rows;
  auto notesPanel = [&](const string& notes) {
    const auto wrapped = wrapUpdateNotes(notes.empty() ? "No release notes were provided for this release." : notes,
                                         max(36, width - 4));
    const size_t maximumScroll = wrapped.size() > static_cast<size_t>(notesHeight)
                                     ? wrapped.size() - static_cast<size_t>(notesHeight)
                                     : 0U;
    const size_t start = min(updateNotesScroll_, maximumScroll);
    ftxui::Elements visible;
    for (size_t index = start; index < min(wrapped.size(), start + static_cast<size_t>(notesHeight)); ++index) {
      visible.push_back(fullLine(wrapped[index], uiPrimaryText(), uiSurfaceBg()));
    }
    if (visible.empty()) visible.push_back(fullLine(" ", uiMutedText(), uiSurfaceBg()));
    auto panel = ftxui::vbox(move(visible)) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, notesHeight) |
                 ftxui::reflect(self->updateNotesBounds_) | ftxui::bgcolor(uiSurfaceBg());
    return panel;
  };

  switch (updateStep_) {
    case UpdateWizardStep::Preparing:
      rows.push_back(uiHeaderText("Preparing your update", uiTitleColor()));
      rows.push_back(styledText(uiLoadingSpinner() + " Checking the latest GitHub release…", uiLinkColor()));
      rows.push_back(styledText("The current Inventatory installation remains available until verification finishes.",
                                uiSecondaryText()));
      break;
    case UpdateWizardStep::SavePrompt:
      rows.push_back(uiHeaderText("Save settings before updating?", uiTitleColor()));
      rows.push_back(styledText("Your Settings page has unsaved changes. Save them before Inventatory restarts.",
                                uiSecondaryText()));
      rows.push_back(ftxui::hbox({target(uiPrimaryButton("Save and update"), "update.save", UiTargetKind::Button,
                                  [self] {
                                    if (self->saveSettingsDraft()) self->beginUpdatePreparation();
                                  }),
                                  ftxui::text("  "),
                                  target(uiSecondaryButton("Cancel"), "update.cancel", UiTargetKind::Button,
                                         [self] { self->cancelSoftwareUpdate(); })}));
      rows.push_back(styledText("[ S ] Save and update   [ Esc ] Cancel", uiMutedText()));
      break;
    case UpdateWizardStep::Preview:
      if (updatePackage_.has_value()) {
        rows.push_back(uiHeaderText("Inventatory update available", uiTitleColor()));
        rows.push_back(styledText("Update to " + updatePackage_->release.latestVersion + " from " + softwareVersion(),
                                  uiLinkColor()));
        rows.push_back(styledText("The complete release package includes Inventatory and the background service.",
                                  uiSecondaryText()));
        rows.push_back(uiSectionHeader("RELEASE NOTES", uiSecondaryText()));
        rows.push_back(notesPanel(previewNotes(updatePackage_->release.releaseNotes)));
        rows.push_back(ftxui::hbox({target(uiPrimaryButton("Start update"), "update.start", UiTargetKind::Button,
                                    [self] { self->beginUpdateDownload(); }),
                                    ftxui::text("  "),
                                    target(uiSecondaryButton("Cancel"), "update.cancel", UiTargetKind::Button,
                                           [self] { self->cancelSoftwareUpdate(); })}));
        rows.push_back(styledText("[ Enter / S ] Start update   [ Esc ] Cancel   [ ↑↓ ] Read notes", uiMutedText()));
      }
      break;
    case UpdateWizardStep::Downloading: {
      string asset = "update package";
      uint64_t downloaded = 0;
      uint64_t total = 0;
      double speed = 0.0;
      if (updateDownloadState_ != nullptr) {
        lock_guard<mutex> lock(updateDownloadState_->mutex);
        asset = updateDownloadState_->assetName.empty() ? asset : updateDownloadState_->assetName;
        downloaded = updateDownloadState_->downloadedBytes;
        total = updateDownloadState_->totalBytes;
        speed = updateDownloadState_->bytesPerSecond;
      }
      rows.push_back(uiHeaderText("Downloading Inventatory " +
                                      (updatePackage_.has_value() ? updatePackage_->release.latestVersion : "update"),
                                  uiTitleColor()));
      rows.push_back(styledText(uiLoadingSpinner() + " " + asset, uiLinkColor()));
      const double fraction = total == 0 ? 0.0 : min(1.0, static_cast<double>(downloaded) / static_cast<double>(total));
      rows.push_back(uiProgressBar(fraction, max(30, min(60, width - 12)), uiLinkColor()));
      rows.push_back(styledText(formatBytes(downloaded) +
                                    (total == 0 ? " downloaded" : " of " + formatBytes(total) + "  " +
                                                                   to_string(static_cast<int>(fraction * 100.0)) + "%"),
                                uiSecondaryText()));
      if (speed > 0.0) rows.push_back(styledText(formatBytes(static_cast<uint64_t>(speed)) + "/s · " +
                                                    formatEta(updateEtaSeconds(downloaded, total, speed)),
                                                uiMutedText()));
      rows.push_back(target(uiSecondaryButton("Cancel"), "update.cancel", UiTargetKind::Button,
                            [self] { self->cancelSoftwareUpdate(); }));
      rows.push_back(styledText("[ Esc ] Cancel until installation handoff", uiMutedText()));
      break;
    }
    case UpdateWizardStep::Verifying:
      rows.push_back(uiHeaderText("Checking the update", uiTitleColor()));
      rows.push_back(styledText(uiLoadingSpinner() + " Verifying SHA-256 checksums", uiLinkColor()));
      rows.push_back(styledText("The package and installer must match the published release hashes.", uiSecondaryText()));
      rows.push_back(target(uiSecondaryButton("Cancel"), "update.cancel", UiTargetKind::Button,
                            [self] { self->cancelSoftwareUpdate(); }));
      rows.push_back(styledText("[ Esc ] Cancel until installation handoff", uiMutedText()));
      break;
    case UpdateWizardStep::HandingOff:
      rows.push_back(uiHeaderText("Finishing the update", uiTitleColor()));
      rows.push_back(styledText(uiLoadingSpinner() + " Handing the verified package to the installer…", uiLinkColor()));
      rows.push_back(styledText("Inventatory will close briefly and restart with the new version.", uiSecondaryText()));
      break;
    case UpdateWizardStep::Complete:
      rows.push_back(uiHeaderText("Inventatory was updated", uiSuccessColor()));
      rows.push_back(styledText("Now running " + (updateCompletionVersion_.empty() ? softwareVersion() : updateCompletionVersion_),
                                uiLinkColor()));
      rows.push_back(uiSectionHeader("COMPLETE RELEASE NOTES", uiSecondaryText()));
      rows.push_back(notesPanel(updateCompletionNotes_));
      rows.push_back(target(uiPrimaryButton("Continue"), "update.continue", UiTargetKind::Button,
                            [self] { self->dismissUpdateResult(); }));
      rows.push_back(styledText("[ Enter ] Continue   [ ↑↓ ] Read notes", uiMutedText()));
      break;
    case UpdateWizardStep::Failed:
      rows.push_back(uiHeaderText("Update could not be completed", uiDangerColor()));
      rows.push_back(styledText(updateError_.empty() ? "Your previous Inventatory installation is still active." : updateError_,
                                uiWarnColor()));
      rows.push_back(styledText("You can retry from this screen or return to Inventatory.", uiSecondaryText()));
      rows.push_back(ftxui::hbox({target(uiPrimaryButton("Retry"), "update.retry", UiTargetKind::Button,
                                  [self] { self->retrySoftwareUpdate(); }),
                                  ftxui::text("  "),
                                  target(uiSecondaryButton("Return"), "update.return", UiTargetKind::Button,
                                         [self] { self->dismissUpdateResult(); })}));
      rows.push_back(styledText("[ R ] Retry   [ Enter / Esc ] Return", uiMutedText()));
      break;
  }
  return ftxui::vbox(move(rows)) | ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, width);
}

ftxui::Element App::renderUpdateUi() const {
  return renderOnboardingFrame(renderUpdateContent());
}

}  // namespace inventatory
