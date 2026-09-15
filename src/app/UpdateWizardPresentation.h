// Inventatory - update wizard presentation contracts.

#pragma once

#include "platform/system/Console.h"

namespace inventatory {

struct UpdatePreviewControlHints {
  const char* releaseNotesHeading = "Release notes";
  const char* start = "[Enter]";
  const char* cancel = "[Esc]";
  const char* readNotes = "[↑↓] Read notes";
};

inline UpdatePreviewControlHints updatePreviewControlHints() {
  return {};
}

inline bool updatePreviewStartsOnKey(const KeyEvent& key) {
  return key.type == KeyType::Enter;
}

}  // namespace inventatory
