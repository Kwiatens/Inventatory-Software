// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Plain-sentence header lines ("Rack 1: take 26 pieces from 7 slots") built from fixed templates,
// so a state always reads the same. Span tone is semantic; the UI maps it to weight and colour.

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

// Pluralises by count: "1 part", "26 parts".
inline std::string voiceCount(int count, const std::string& singular, const std::string& plural) {
  return std::to_string(count) + " " + (count == 1 ? singular : plural);
}

}  // namespace inventatory
