// Inventatory - Private label-printer implementation contracts.

#pragma once

#include "label_printer/LabelPrinter.h"

#include <ctime>
#include <initializer_list>

namespace inventatory {
namespace label_printer_detail {

using std::initializer_list;

string uppercaseAscii(string value);
string lowerAscii(string value);
string shortCode(const string& value, size_t maxLength = 14);
string fieldOrBlank(const string& value, size_t maxLength);
string sanitiseZplFragment(const string& value);
string shortParameterLabel(const string& label);
string shortValueLine(const string& label, optional<string> value, size_t maxLength = 24);
string compactDescriptor(const string& value, size_t maxLength = 10);
string dateOnly(time_t value);
string compactJoin(const vector<string>& parts, const string& separator);
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

bool isCompactManufacturerPartNumber(const string& value);
CableFlagFont cableFlagFont(const string& text);
vector<string> wrapLabelLines(const string& value, size_t maxWidth, size_t maxLines);
string collectLineFromValues(initializer_list<string> values, const string& separator, size_t maxLength);
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
vector<string> itemTextTokens(const InventoryItem& item);
bool itemTextHasToken(const InventoryItem& item, initializer_list<const char*> tokens);
bool hasParameter(const InventoryItem& item, initializer_list<const char*> names);

string sensorContextHeader(const InventoryItem& item);
string diodeContextHeader(const InventoryItem& item);
bool startsWithInsensitive(const string& value, const string& prefix);
string diodeMainLabelValue(const InventoryItem& item);
bool isIcLikeItem(const InventoryItem& item);
string powerIcContextHeader(const InventoryItem& item);
string analogIcContextHeader(const InventoryItem& item);
string sensorIcContextHeader(const InventoryItem& item);
string dataConverterContextHeader(const InventoryItem& item);
string timingIcContextHeader(const InventoryItem& item);
string driverIcContextHeader(const InventoryItem& item);
string logicIcContextHeader(const InventoryItem& item);
string memoryIcContextHeader(const InventoryItem& item);
string integratedCircuitContextHeader(const InventoryItem& item);
string transistorContextHeader(const InventoryItem& item);
string fallbackContextHeader(const InventoryItem& item);

string mainLabelValue(const InventoryItem& item);
string shortPackageLine(const InventoryItem& item);
string manufacturerLine(const InventoryItem& item);
string normalizedShieldingLine(const optional<string>& value);
vector<string> fallbackDetailLines(const InventoryItem& item, size_t maxLines);
vector<string> parameterLinesForItem(const InventoryItem& item);

string makeJobName(const InventoryItem& item);
string rackDisplayCategory(string value);
string rackLabelText(const string& code);
vector<string> rackCategoryLines(const string& category);
string rackCategoryFieldData(const vector<string>& lines);
string makeRackJobName(const InventatoryRack& rack);
string partContextHeader(const InventoryItem& item);

}  // namespace label_printer_detail
}  // namespace inventatory
