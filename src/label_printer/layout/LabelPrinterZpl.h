// Inventatory - ZPL drawing helpers shared by the part and rack labels.
//
// Labels are 256 x 200 dots (32 x 25 mm, 203 dpi). Test prints land about 3 dots right of and 10 dots below the
// ^FO origin, so layouts start at x 7 / y 0 and end at x 243 / y 179, which leaves an even margin of about 10
// dots on every side of the printed label.

#pragma once

#include "label_printer/core/LabelPrinterPrivate.h"
#include "label_printer/symbols/RackSymbols.h"

#include <iomanip>
#include <sstream>

namespace inventatory {
namespace label_printer_detail {

inline constexpr int kLabelLeft = 7;
inline constexpr int kLabelWidth = 236;
inline constexpr int kLabelBottom = 179;
inline constexpr int kHeaderHeight = 22;
// Font 0 digits and capitals are about 0.74 of the font height tall and start at the field origin, so their
// baseline is at y + 0.74 * height.
inline constexpr double kCapHeight = 0.74;

// The >| brand mark as a 5 x 5 pixel glyph, drawn as 3 dot squares.
inline constexpr const char* kLogoRows[] = {"X...X", ".X..X", "..X.X", ".X..X", "X...X"};
inline constexpr int kLogoInset = 4;
inline constexpr int kTitleInset = 6;
inline constexpr int kLogoSize = 15;

inline int scaledDots(int value, double factor) {
  return static_cast<int>(value * factor + 0.5);
}

// Top of a font 0 line whose capitals are centred on centerY.
inline int centeredTop(double centerY, int size) {
  return static_cast<int>(centerY - size * kCapHeight / 2 + 0.5);
}

// Left edge that centres a text of the given estimated width in a box.
inline int centeredLeft(int boxX, int boxWidth, int textWidth) {
  return boxX + (boxWidth - textWidth) / 2;
}

inline void writeText(std::ostringstream& out, int x, int y, int height, int width, const string& text,
                      const string& extra = {}) {
  out << "^FO" << x << ',' << y << "^A0N," << height << ',' << width << extra << "^FD" << sanitizeLabelText(text)
      << "^FS\r\n";
}

inline void writeBox(std::ostringstream& out, int x, int y, int width, int height, int thickness, int rounding = 0,
                     bool reverse = false) {
  out << "^FO" << x << ',' << y << (reverse ? "^FR" : "") << "^GB" << width << ',' << height << ',' << thickness;
  if (rounding > 0) out << ",B," << rounding;
  out << "^FS\r\n";
}

// A 1-bit symbol as an ASCII hex graphic field (^GFA): 1 = black.
inline void writeGraphic(std::ostringstream& out, int x, int y, const RackSymbolBitmap& bitmap) {
  const auto bytes = static_cast<size_t>(bitmap.bytesPerRow) * static_cast<size_t>(bitmap.height);
  out << "^FO" << x << ',' << y << "^GFA," << bytes << ',' << bytes << ',' << bitmap.bytesPerRow << ',';
  out << std::uppercase << std::hex << std::setfill('0');
  for (size_t index = 0; index < bytes; ++index) out << std::setw(2) << static_cast<int>(bitmap.data[index]);
  out << std::dec << std::nouppercase << std::setfill(' ') << "^FS\r\n";
}

// The black header bar: title at the left, brand mark at the right end. Returns the title's width budget.
inline int writeBrandHeader(std::ostringstream& out, const string& title) {
  writeBox(out, kLabelLeft, 0, kLabelWidth, kHeaderHeight, kHeaderHeight, 4);
  const auto logoX = kLabelLeft + kLabelWidth - kLogoInset - kLogoSize;
  for (int row = 0; row < 5; ++row) {
    for (int column = 0; column < 5; ++column) {
      if (kLogoRows[row][column] == 'X') writeBox(out, logoX + column * 3, 4 + row * 3, 3, 3, 3, 0, true);
    }
  }
  const auto titleX = kLabelLeft + kTitleInset;
  const auto budget = logoX - 7 - titleX;
  const auto fitted = fitFont0Text(title, budget, {16, 15, 14, 13, 12});
  if (!fitted.text.empty()) {
    writeText(out, titleX, centeredTop(kHeaderHeight / 2.0, fitted.size), fitted.size, fitted.size, fitted.text,
              "^FR");
  }
  return budget;
}

}  // namespace label_printer_detail
}  // namespace inventatory
