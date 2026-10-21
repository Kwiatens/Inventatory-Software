// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

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
  // Identity columns from EasyEDA exports. Empty for KiCad BOMs.
  string supplier;               // "LCSC", "DigiKey"
  string supplierPartNumber;     // "C25744" for LCSC, a DigiKey part number for DigiKey
  string manufacturerPartNumber; // "CL05B104KO5NNNC"
  // Quantity disagrees with the designator count; the row needs confirmation before matching.
  bool quantityConflict = false;
};

struct KicadBomFile {
  bool ok = false;
  string error;
  string projectName;
  vector<BomLine> lines;
  vector<string> warnings;  // rows skipped or flagged while parsing
};

// True for test points, mounting holes, fiducials and logos: rows that are never bought.
bool isNonOrderableDesignator(const string& designator, const string& footprint);

KicadBomFile parseKicadBomText(const string& text, const string& projectName);
KicadBomFile loadKicadBomFile(const filesystem::path& path);

// Human-friendly project name derived from a BOM filename stem.
string projectNameFromPath(const filesystem::path& path);

}  // namespace inventatory
