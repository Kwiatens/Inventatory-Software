// Inventatory - Private label-printer implementation contracts.

#pragma once

#include "label_printer/core/LabelPrinter.h"

#include <ctime>
#include <initializer_list>

namespace inventatory {
namespace label_printer_detail {

using std::initializer_list;

// Width in dots available to a parameter tile value on the 32 x 25 mm label.
inline constexpr int kLabelTileWidth = 78;
// The smallest size a tile value is printed at; longer values are left off.
inline constexpr int kLabelTileMinSize = 13;

string uppercaseAscii(string value);
string lowerAscii(string value);
string shortCode(const string& value, size_t maxLength = 14);
string fieldOrBlank(const string& value, size_t maxLength);
string sanitiseZplFragment(const string& value);
string shortParameterLabel(const string& label);
string compactDescriptor(const string& value, size_t maxLength = 10);
string normalizeResistanceValue(string value);
string fitSingleLineLabel(const string& value, size_t maxLength);

struct CableFlagFont {
  int height;
  int width;
};

struct SingleLineFont {
  int height;
  int width;
};

// Font 0 text fitted to a box: the largest candidate size whose estimated
// width fits, or the smallest size with an ellipsized string.
struct FittedLabelText {
  string text;
  int size = 0;
  int width = 0;
};

int estimateFont0Width(const string& text, int height, int width);
FittedLabelText fitFont0Text(const string& text, int maxWidth, initializer_list<int> sizes);

CableFlagFont cableFlagFont(const string& text);
bool looksLikeFrequencyValue(const string& value);
bool looksLikeInductanceValue(const string& value);
string canonicalInductanceUnit(string unit);
optional<string> extractInductanceFromText(const string& text);
optional<string> parameterValueMatching(const InventoryItem& item,
                                        initializer_list<const char*> names,
                                        bool (*predicate)(const string&));
optional<string> firstParameter(const InventoryItem& item, initializer_list<const char*> names);
optional<string> firstInductanceParameter(const InventoryItem& item);
bool itemTextContains(const InventoryItem& item, initializer_list<const char*> needles);
bool hasParameter(const InventoryItem& item, initializer_list<const char*> names);

bool startsWithInsensitive(const string& value, const string& prefix);
string diodeMainLabelValue(const InventoryItem& item);
bool isIcLikeItem(const InventoryItem& item);

bool isMeasuredValueItem(const InventoryItem& item);
string mainLabelValue(const InventoryItem& item);
string mainLabelTolerance(const InventoryItem& item);
string shortPackageLine(const InventoryItem& item);
string manufacturerLine(const InventoryItem& item);
// Manufacturer names from fullest to shortest: "Infineon Technologies", "Infineon".
vector<string> manufacturerCandidates(const string& name);
string normalizedShieldingLine(const optional<string>& value);
string labelTileValue(const string& value);
void splitMeasuredValue(const string& value, string& number, string& unit);
void splitRackLocation(const string& location, string& rackCode, string& rackCell);
vector<LabelParameterTile> fallbackParameterTiles(const InventoryItem& item, size_t maxTiles);
vector<LabelParameterTile> parameterTilesForItem(const InventoryItem& item);

string makeJobName(const InventoryItem& item);
string rackDisplayCategory(string value);
string rackNumberText(const string& code);
string rackShortCategory(const string& componentType);
string makeRackJobName(const InventatoryRack& rack);
string partContextHeader(const InventoryItem& item);

}  // namespace label_printer_detail

std::unique_ptr<PrinterBackend> createPlatformPrinterBackend();
}  // namespace inventatory
