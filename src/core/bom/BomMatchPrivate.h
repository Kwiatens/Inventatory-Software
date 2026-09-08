#pragma once

#include "core/bom/BomMatch.h"

#include <initializer_list>
#include <string>
#include <vector>

namespace inventatory::bom_match_detail {

inline const std::initializer_list<const char*> kChipCodes = {
    "01005", "0201", "0402", "0603", "0805", "1206", "1210", "1806", "1812", "2010", "2512"};

inline const std::initializer_list<const char*> kPackageFamilies = {
    "HTSSOP", "TSSOP", "TSOT", "SSOP", "MSOP", "SOIC", "SOT", "SOD", "SOP", "QFN", "DFN",
    "TQFP",   "LQFP",  "QFP",  "BGA",  "WSON", "VSON", "SC",  "TO",  "DIP", "PDIP"};

std::vector<std::string> splitOnUnderscore(const std::string& value);
bool startsWithInsensitive(const std::string& value, const std::string& prefix);
std::string diameterToken(const std::vector<std::string>& tokens);
std::string pinCountToken(const std::vector<std::string>& tokens);
std::optional<double> parseNumberWithMultiplier(const std::string& body, bool resistanceLike);
bool endsWith(const std::string& value, const std::string& suffix);
std::string compactKey(const std::string& value);
std::optional<double> itemValueFor(const InventoryItem& item, ValueKind kind);
std::optional<std::string> itemPackage(const InventoryItem& item);
bool sameText(const std::string& lhs, const std::string& rhs);
bool partNameHasToken(const InventoryItem& item, const std::string& designation);
int scoreItem(const InventoryItem& item, const BomLine& line, const std::string& bomPackage,
              std::optional<double> bomValue, ValueKind bomKind);
int saturatingAdd(int lhs, int rhs);
int saturatingMultiplyNonNegative(int lhs, int rhs);

}  // namespace inventatory::bom_match_detail
