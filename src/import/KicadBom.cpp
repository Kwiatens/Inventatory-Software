// Inventatory - Hardware Inventory Management System
// KiCad grouped BOM parsing.

#include "import/KicadBom.h"

#include "core/Inventory.h"
#include "import/CsvReader.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>

namespace inventatory {

using namespace std;

namespace {

struct BomColumns {
  int designator = -1;
  int footprint = -1;
  int designation = -1;
  int quantity = -1;
  int supplier = -1;
};

BomColumns mapBomColumns(const vector<string>& headers) {
  BomColumns columns;
  columns.designator = findColumn(headers, {"Designator", "Designators", "Reference", "References", "Ref", "Refs"});
  columns.footprint = findColumn(headers, {"Footprint", "Package", "Footprint Name"});
  columns.designation = findColumn(headers, {"Designation", "Value", "Comment"});
  columns.quantity = findColumn(headers, {"Quantity", "Qty", "Quantity Per PCB"});
  columns.supplier = findColumn(headers, {"Supplier and ref", "Supplier", "Supplier and ref."});
  return columns;
}

optional<int> parseCount(const string& value) {
  const auto trimmed = trim(value);
  if (trimmed.empty()) {
    return nullopt;
  }
  int result = 0;
  for (unsigned char ch : trimmed) {
    if (!isdigit(ch)) {
      return nullopt;
    }
    const int digit = static_cast<int>(ch - '0');
    if (result > (2147483647 - digit) / 10) {
      return nullopt;
    }
    result = result * 10 + digit;
  }
  return result;
}

vector<string> splitDesignators(const string& value) {
  vector<string> designators;
  string current;
  const auto flush = [&] {
    const auto trimmed = trim(current);
    if (!trimmed.empty()) {
      designators.push_back(trimmed);
    }
    current.clear();
  };

  for (const char ch : value) {
    if (ch == ',' || ch == ';' || ch == ' ' || ch == '\t') {
      flush();
    } else {
      current.push_back(ch);
    }
  }
  flush();
  return designators;
}

// Reference prefixes that never map to a purchasable part.
bool nonOrderablePrefix(const string& designator) {
  static const initializer_list<const char*> kPrefixes = {"tp", "h", "mh", "fid", "nt", "logo", "mk"};
  const auto lowered = toLower(trim(designator));
  if (lowered.empty()) {
    return false;
  }

  for (const auto* prefix : kPrefixes) {
    const string token(prefix);
    if (lowered.rfind(token, 0) != 0 || lowered.size() == token.size()) {
      continue;
    }
    // Only treat it as a reference prefix when digits follow, so "H1" is a
    // mounting hole while "HDR" stays a real part.
    const auto suffix = lowered.substr(token.size());
    if (all_of(suffix.begin(), suffix.end(), [](unsigned char ch) { return isdigit(ch) != 0; })) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool isNonOrderableDesignator(const string& designator, const string& footprint) {
  const auto ref = trim(designator);
  if (ref.empty() || ref.rfind("REF**", 0) == 0 || ref == "*") {
    return true;
  }
  if (nonOrderablePrefix(ref)) {
    return true;
  }

  const auto loweredFootprint = toLower(footprint);
  static const initializer_list<const char*> kFootprints = {"testpoint", "mountinghole", "fiducial", "logo",
                                                            "netie", "solderjumper"};
  return any_of(kFootprints.begin(), kFootprints.end(), [&](const char* needle) {
    string compact;
    compact.reserve(loweredFootprint.size());
    for (unsigned char ch : loweredFootprint) {
      if (isalnum(ch)) {
        compact.push_back(static_cast<char>(ch));
      }
    }
    return compact.find(needle) != string::npos;
  });
}

KicadBomFile parseKicadBomText(const string& text, const string& projectName) {
  KicadBomFile bom;
  bom.projectName = projectName;

  const auto cleaned = stripByteOrderMark(text);
  string parseError;
  const auto rows = parseCsv(cleaned, sniffDelimiter(cleaned), parseError);
  if (!parseError.empty()) {
    bom.error = parseError;
    return bom;
  }
  if (rows.size() < 2) {
    bom.error = "BOM does not contain a header and component rows";
    return bom;
  }

  const auto columns = mapBomColumns(rows.front());
  if (columns.designator < 0 || columns.designation < 0) {
    bom.error = "BOM is missing the Designator or Designation column";
    return bom;
  }

  for (size_t rowIndex = 1; rowIndex < rows.size(); ++rowIndex) {
    const auto& row = rows[rowIndex];
    const auto sourceRow = rowIndex + 1;

    BomLine line;
    line.sourceRow = sourceRow;
    line.designators = splitDesignators(csvCell(row, columns.designator));
    line.footprint = csvCell(row, columns.footprint);
    line.designation = csvCell(row, columns.designation);
    line.supplierRef = csvCell(row, columns.supplier);
    // KiCad usually writes an explicit Quantity, but ungrouped exports omit it.
    line.quantityPerBoard =
        parseCount(csvCell(row, columns.quantity)).value_or(static_cast<int>(line.designators.size()));

    if (line.designators.empty() || line.designation.empty()) {
      continue;
    }

    const bool excluded = all_of(line.designators.begin(), line.designators.end(), [&](const string& ref) {
      return isNonOrderableDesignator(ref, line.footprint);
    });
    if (excluded) {
      bom.warnings.push_back("Row " + to_string(sourceRow) + ": " + line.designation + " is not orderable");
      continue;
    }

    if (line.quantityPerBoard <= 0) {
      bom.warnings.push_back("Row " + to_string(sourceRow) + ": " + line.designation + " has no quantity");
      continue;
    }

    bom.lines.push_back(move(line));
  }

  if (bom.lines.empty()) {
    bom.error = "BOM headers were recognized, but no orderable component rows were found";
    return bom;
  }

  bom.ok = true;
  return bom;
}

string projectNameFromPath(const filesystem::path& path) {
  auto stem = path.stem().string();
  replace(stem.begin(), stem.end(), '_', ' ');
  replace(stem.begin(), stem.end(), '-', ' ');
  const auto trimmed = trim(stem);
  return trimmed.empty() ? string("Untitled project") : trimmed;
}

KicadBomFile loadKicadBomFile(const filesystem::path& path) {
  ifstream input(path, ios::binary);
  if (!input) {
    KicadBomFile bom;
    bom.error = "Unable to open BOM file";
    return bom;
  }

  ostringstream buffer;
  buffer << input.rdbuf();
  return parseKicadBomText(buffer.str(), projectNameFromPath(path));
}

}  // namespace inventatory
