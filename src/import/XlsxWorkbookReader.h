// Inventatory - native XLSX table reader for local manufacturer catalogues.
#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace inventatory {

// A deliberately small common representation: CSV and XLSX both produce this
// before profile mapping, normalization, and SQLite storage begin.
struct CatalogueTable {
  std::string sheetName;
  std::vector<std::string> headers;
  std::vector<std::vector<std::string>> rows;
  std::size_t headerRow = 0;
  std::size_t sourceRowCount = 0;
  char delimiter = ',';
  std::string encoding = "UTF-8";
};

// Reads a worksheet without starting Excel or another spreadsheet program.
// `worksheetPattern` is a case-insensitive hint; an empty hint selects the
// first non-empty worksheet.  Formula cells use their cached result when it is
// present.  Numeric identifiers formatted with zero placeholders retain those
// leading zeroes.
bool readXlsxCatalogueTable(const std::filesystem::path& file, const std::string& worksheetPattern,
                            CatalogueTable& table, std::string& error, std::atomic_bool* cancel = nullptr,
                            const std::function<void(std::size_t, std::size_t, const std::string&)>& progress = {});

}  // namespace inventatory
