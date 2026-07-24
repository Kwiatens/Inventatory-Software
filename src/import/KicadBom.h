// Inventatory - Hardware Inventory Management System
// KiCad grouped BOM parsing.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace inventatory {

namespace filesystem = std::filesystem;
using std::string;
using std::vector;

// One grouped BOM row: every designator sharing a value and footprint.
struct BomLine {
  size_t sourceRow = 0;
  vector<string> designators;  // "C1, C15, C16" split into individual refs
  string footprint;            // "C_0603_1608Metric"
  string designation;          // "1uF", "ESP32-S3-WROOM-1"
  int quantityPerBoard = 0;
  string supplierRef;
};

struct KicadBomFile {
  bool ok = false;
  string error;
  string projectName;
  vector<BomLine> lines;
  vector<string> warnings;  // rows excluded as non-orderable
};

// True for test points, mounting holes, fiducials and logos: real BOM rows that
// never correspond to a purchasable part.
bool isNonOrderableDesignator(const string& designator, const string& footprint);

KicadBomFile parseKicadBomText(const string& text, const string& projectName);
KicadBomFile loadKicadBomFile(const filesystem::path& path);

// Human-friendly project name derived from a BOM filename stem.
string projectNameFromPath(const filesystem::path& path);

}  // namespace inventatory
