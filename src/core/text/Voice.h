// Inventatory - Hardware Inventory Management System
// Plain-sentence header lines ("Open Rack 1 and take 26 parts from 7 slots").
//
// A voice line is built from fixed templates filled with live data, so the same state always reads
// the same way and every template can be tested as plain text. The tone of each span is semantic;
// the UI maps it to weight and colour.

#pragma once

#include <string>
#include <vector>

namespace inventatory {

enum class VoiceTone {
  Plain,    // ordinary words
  Muted,    // qualifiers and facts that need no attention
  Strong,   // the counts and names the user acts on
  Slot,     // a rack slot or rack name to go to
  Success,
  Warning,
  Danger,
};

struct VoiceSpan {
  std::string text;
  VoiceTone tone = VoiceTone::Plain;
};

using VoiceLine = std::vector<VoiceSpan>;

inline std::string voiceText(const VoiceLine& line) {
  std::string text;
  for (const auto& span : line) text += span.text;
  return text;
}

// "1 part" / "26 parts".
inline std::string voiceCount(int count, const std::string& singular, const std::string& plural) {
  return std::to_string(count) + " " + (count == 1 ? singular : plural);
}

}  // namespace inventatory
