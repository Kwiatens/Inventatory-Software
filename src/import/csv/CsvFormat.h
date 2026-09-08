// Inventatory - Hardware Inventory Management System
// Header-only sniffing that tells the importer which CSV dialect arrived.

#pragma once

#include <string>

namespace inventatory {

using std::string;

enum class CsvFormat {
  Unknown,
  DigiKeyOrder,
  KicadBom,
};

// Reads only the header row, so detection stays cheap and never depends on
// whether the file happens to contain valid product lines.
CsvFormat detectCsvFormat(const string& text);

string csvFormatName(CsvFormat format);

}  // namespace inventatory
