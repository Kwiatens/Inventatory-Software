// Inventatory - update wizard presentation contracts.

#pragma once

#include "platform/system/Console.h"

#include <string>

namespace inventatory {

struct UpdatePreviewControlHints {
  const char* title = "Inventatory Update Available";
  const char* releaseNotesHeading = "Release notes";
  const char* start = "[Enter]";
  const char* cancel = "[Esc]";
  const char* readNotes = "[↑↓] Read notes";
};

inline UpdatePreviewControlHints updatePreviewControlHints() {
  return {};
}

inline std::string updatePreviewVersionLine(const std::string& installedVersion,
                                            const std::string& newVersion) {
  return installedVersion + " -> " + newVersion;
}

inline bool updatePreviewStartsOnKey(const KeyEvent& key) {
  return key.type == KeyType::Enter;
}

}  // namespace inventatory
