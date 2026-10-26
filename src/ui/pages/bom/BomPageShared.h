// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Pieces every Projects screen draws its tables and headers with.

#pragma once

#include "core/bom/BomVendorMatch.h"
#include "core/text/AsciiFold.h"
#include "core/text/Voice.h"
#include "ui/shared/AppUiShared.h"

#include <string>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>

namespace inventatory::bom_page {

using std::string;
using std::vector;
using std::pair;
using std::max;
using std::move;

// A text cell of exact width; bold for the figures a row is read by.
inline ftxui::Element cell(const string& text, int width, ftxui::Color color, bool bold = false, bool rightAlign = false) {
  const auto clipped = ellipsize(text, static_cast<size_t>(max(0, width - 1)));
  auto content = bold ? uiHeaderText(clipped, color) : styledText(clipped, color);
  auto row = rightAlign ? ftxui::hbox({ftxui::filler(), move(content), ftxui::text(" ")})
                        : ftxui::hbox({move(content), ftxui::filler()});
  return row | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

// A data table cell sized by tableColumn: the text, then the column gap. A number sits flush right before its gap,
// so a column of numbers lines up under its right-aligned heading.
inline ftxui::Element tableCell(const string& text, int width, ftxui::Color color, bool bold = false, bool rightAlign = false) {
  const auto clipped = ellipsize(text, static_cast<size_t>(max(0, width - kColumnGap)));
  auto content = bold ? uiHeaderText(clipped, color) : styledText(clipped, color);
  auto row = rightAlign ? ftxui::hbox({ftxui::filler(), move(content), ftxui::text(string(kColumnGap, ' '))})
                        : ftxui::hbox({move(content), ftxui::filler()});
  return row | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
}

// Vendor text in ASCII, as every rendered string must be: "100 µF" -> "100 uF", "±1%" -> "+/-1%",
// "Würth" -> "Wurth".
inline string asciiVendorText(const string& text) { return foldToAscii(text); }

// The parameters that tell parts in a picker apart, joined: "1 uF  0603  X7R  16V".
inline string pickerDetails(const VendorCandidate& candidate) {
  string text;
  for (const char* label : {"Resistance", "Capacitance", "Inductance", "Frequency", "Color", "Current Rating (Amps)",
                            "Number of Positions", "Package / Case", "Tolerance", "Temperature Coefficient",
                            "Voltage - Rated"}) {
    auto value = trim(vendorParameter(candidate, label));
    if (value.empty() || value == "-") continue;
    // "0805 (2012 Metric)" reads as "0805": the metric code only repeats the size.
    if (string(label) == "Package / Case") {
      if (const auto metric = value.find(" ("); metric != string::npos && value.find("Metric)", metric) != string::npos) {
        value.erase(metric);
      }
    }
    text += (text.empty() ? "" : "  ") + asciiVendorText(value);
  }
  return text.empty() ? asciiVendorText(candidate.description) : text;
}

// A voice line used as a page title: every word is bold, slots keep their focus colour.
inline ftxui::Element voiceTitle(const VoiceLine& line) {
  ftxui::Elements spans;
  for (const auto& span : line) {
    spans.push_back(uiHeaderText(span.text, span.tone == VoiceTone::Slot ? uiFocusColor() : uiPrimaryText()));
  }
  return ftxui::hbox(move(spans));
}

inline ftxui::Element buttonGroup(ftxui::Elements buttons) {
  ftxui::Elements spaced;
  for (auto& button : buttons) {
    if (!spaced.empty()) spaced.push_back(ftxui::text(" "));
    spaced.push_back(move(button));
  }
  return ftxui::hbox(move(spaced));
}

}  // namespace inventatory::bom_page
