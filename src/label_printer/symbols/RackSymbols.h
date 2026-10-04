// Inventatory - Electrical symbols printed on rack labels.
//
// The bitmaps are generated from open-licensed SVG artwork by tools/rack_symbols/generate.py. Only the
// resistor and the fuse differ between the IEC (EU) and ANSI/IEEE (US) sets.

#pragma once

#include <cstddef>
#include <string>

namespace inventatory {

enum class SymbolStandard { Eu, Us };

// 1 bit per dot, rows padded to whole bytes, most significant bit first, 1 = black.
struct RackSymbolBitmap {
  int width;
  int height;
  int bytesPerRow;
  const unsigned char* data;
};

struct RackSymbolSet {
  const char* key;
  const RackSymbolBitmap* eu;
  const RackSymbolBitmap* us;
};

extern const RackSymbolSet kRackSymbolSets[];
extern const size_t kRackSymbolSetCount;

// Symbol key for a rack's component type ("Resistors", "resistor", "ICs"); "grid" for anything custom.
std::string rackSymbolKey(const std::string& componentType);
const RackSymbolBitmap& rackSymbolFor(const std::string& componentType, SymbolStandard standard);

const char* symbolStandardKey(SymbolStandard standard);
// Accepts "eu" or "us" (any case); returns false and leaves the value untouched for anything else.
bool parseSymbolStandard(const std::string& text, SymbolStandard& standard);
const char* symbolStandardLabel(SymbolStandard standard);

}  // namespace inventatory
