// Inventatory - DigiKey product detail extraction.

#include "platform/digikey/DigiKeyApiPrivate.h"

#ifdef _WIN32

#include "core/parts/PartDescriptor.h"
#include "platform/system/Environment.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <regex>
#include <sstream>
#include <utility>

namespace inventatory {
using namespace std;
namespace digikey_detail {

optional<string> extractComponentPackageFromText(const string& text) {
  if (text.empty()) {
    return nullopt;
  }

  static const pair<const char*, const char*> kPatterns[] = {
      {R"(\b(AXIAL|RADIAL|THROUGH HOLE|SURFACE MOUNT|SMD|SMT|MODULE)\b)", "$1"},
      {R"(\b(01005|0201|0402|0603|0805|1206|1210|1812|2010|2512)\b)", "$1"},
      {R"(\b(SOT-?23(?:-?\d+)?)\b)", "$1"},
      {R"(\b(SOT-?223(?:-?\d+)?)\b)", "$1"},
      {R"(\b(SOIC-?\d+)\b)", "$1"},
      {R"(\b(TSSOP-?\d+)\b)", "$1"},
      {R"(\b(SSOP-?\d+)\b)", "$1"},
      {R"(\b(MSOP-?\d+)\b)", "$1"},
      {R"(\b(QFN-?\d+)\b)", "$1"},
      {R"(\b(DFN-?\d+)\b)", "$1"},
      {R"(\b(QFP-?\d+)\b)", "$1"},
      {R"(\b(TQFP-?\d+)\b)", "$1"},
      {R"(\b(LQFP-?\d+)\b)", "$1"},
      {R"(\b(DIP-?\d+)\b)", "$1"},
      {R"(\b(BGA-?\d+)\b)", "$1"},
      {R"(\b(LGA-?\d+)\b)", "$1"},
      {R"(\b(TO-?92(?:-?\d+)?)\b)", "$1"},
      {R"(\b(TO-?220(?:-?\d+)?)\b)", "$1"},
      {R"(\b(TO-?263(?:-?\d+)?)\b)", "$1"},
  };

  for (const auto& [pattern, replacement] : kPatterns) {
    regex re(pattern, regex_constants::icase);
    smatch match;
    if (regex_search(text, match, re) && match.size() > 1) {
      auto package = match[1].str();
      return package;
    }
  }

  return nullopt;
}

bool looksLikeInductorText(const string& text) {
  const auto normalized = normalizeParameterKey(text);
  return normalized.find("inductor") != string::npos || normalized.find("fixedind") != string::npos ||
         normalized.find("choke") != string::npos || normalized.find("coil") != string::npos;
}

string canonicalInductanceUnit(string unit) {
  transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char ch) {
    return static_cast<char>(tolower(ch));
  });
  if (unit == "uh") {
    return "uH";
  }
  if (unit == "nh") {
    return "nH";
  }
  if (unit == "mh") {
    return "mH";
  }
  if (unit == "ph") {
    return "pH";
  }
  return "H";
}

optional<string> extractInductanceFromText(const string& text) {
  if (!looksLikeInductorText(text)) {
    return nullopt;
  }

  regex valuePattern(R"(\b(\d+(?:\.\d+)?|\d+[rR]\d+)\s*([munp]?h)\b)", regex_constants::icase);
  smatch match;
  if (regex_search(text, match, valuePattern) && match.size() > 2) {
    auto number = match[1].str();
    replace(number.begin(), number.end(), 'R', '.');
    replace(number.begin(), number.end(), 'r', '.');
    return number + canonicalInductanceUnit(match[2].str());
  }

  return nullopt;
}

optional<string> extractComponentPackage(const JsonPtr& product) {
  const auto fromParameter = [&](initializer_list<const char*> names) -> optional<string> {
    if (const auto value = readParameterValue(product, names); value.has_value() && !looksLikePackagingType(*value)) {
      return value;
    }
    return nullopt;
  };

  if (const auto package = fromParameter({"Package / Case", "Package Case", "Case / Package", "Case Package",
                                          "Supplier Device Package", "Device Package"});
      package.has_value()) {
    return package;
  }

  if (const auto package = fromParameter({"Package"}); package.has_value()) {
    return package;
  }

  const auto description = readPath(product, {"Description", "ProductDescription"}).value_or("");
  const auto detailedDescription = readPath(product, {"Description", "DetailedDescription"}).value_or("");
  const auto manufacturerPartNumber = readPath(product, {"ManufacturerProductNumber"}).value_or("");
  const auto combinedText = description + " " + detailedDescription + " " + manufacturerPartNumber;
  if (const auto inferred = extractComponentPackageFromText(combinedText); inferred.has_value()) {
    return inferred;
  }

  return nullopt;
}

vector<Parameter> extractParameters(const JsonPtr& product) {
  vector<Parameter> parameters;
  const auto* entries = asArray(findMember(product, "Parameters") == nullptr ? nullptr : *findMember(product, "Parameters"));
  if (entries == nullptr) {
    return parameters;
  }

  for (const auto& entry : *entries) {
    if (parameters.size() >= 256U) break;
    const auto label = readFirstMember(entry, {"Parameter", "ParameterText"});
    const auto value = readParameterText(entry, label.value_or(""));
    if (label.has_value() && value.has_value()) {
      auto labelText = trimCopy(*label);
      auto valueText = trimCopy(*value);
      if (toLower(labelText) == "package") {
        labelText = "Packaging";
      }
      if (!labelText.empty() && !valueText.empty()) {
        parameters.push_back({move(labelText), move(valueText)});
      }
    }
  }

  return parameters;
}

void upsertExtractedInductance(vector<Parameter>& parameters, const string& value) {
  const auto trimmed = trimCopy(value);
  if (trimmed.empty() || !looksLikeInductanceValue(trimmed)) {
    return;
  }

  for (auto& parameter : parameters) {
    const auto label = normalizeParameterKey(parameter.name);
    if (label.find("inductance") != string::npos) {
      if (!looksLikeInductanceValue(parameter.value)) {
        parameter.value = trimmed;
      }
      return;
    }
  }

  parameters.push_back({"Inductance", trimmed});
}

optional<string> extractPackagingType(const JsonPtr& product) {
  if (const auto* variations = asArray(findMember(product, "ProductVariations") == nullptr ? nullptr : *findMember(product, "ProductVariations"));
      variations != nullptr && !variations->empty()) {
    const auto package = readPath((*variations)[0], {"PackageType", "Name"});
    if (package.has_value() && !trimCopy(*package).empty()) {
      return package;
    }
  }
  return nullopt;
}

DigiKeyProductDetails parseProductDetails(const string& lookupKey, const JsonPtr& root) {
  DigiKeyProductDetails details;
  details.lookupKey = lookupKey;

  const auto* product = findMember(root, "Product");
  const JsonPtr productNode = product != nullptr ? *product : root;
  const auto categoryPath = extractCategoryPath(productNode);

  details.manufacturerName = readPath(productNode, {"Manufacturer", "Name"}).value_or("");
  details.manufacturerPartNumber = readPath(productNode, {"ManufacturerProductNumber"}).value_or("");
  if (!categoryPath.empty()) {
    details.categoryName = categoryPath.back();
  }
  details.productDescription = readPath(productNode, {"Description", "ProductDescription"}).value_or("");
  details.detailedDescription = readPath(productNode, {"Description", "DetailedDescription"}).value_or("");
  details.productUrl = readPath(productNode, {"ProductUrl"}).value_or("");
  details.datasheetUrl = readPath(productNode, {"DatasheetUrl"}).value_or("");
  if (details.datasheetUrl.empty()) {
    details.datasheetUrl = readPath(productNode, {"PrimaryDatasheet"}).value_or("");
  }
  details.rohsStatus = readPath(productNode, {"RoHSStatus"}).value_or("");
  details.leadStatus = readPath(productNode, {"LeadStatus"}).value_or("");
  details.productStatus = readPath(productNode, {"ProductStatus", "Status"}).value_or("");
  details.manufacturerLeadWeeks = readPath(productNode, {"ManufacturerLeadWeeks"}).value_or("");
  details.quantityAvailable = readPath(productNode, {"QuantityAvailable"}).value_or("");

  if (const auto package = extractPackagingType(productNode); package.has_value()) {
    details.packagingType = *package;
  }

  if (const auto package = extractComponentPackage(productNode); package.has_value()) {
    details.packageName = *package;
  }

  const auto standardPricing = findMember(productNode, "StandardPricing");
  if (const auto* pricing = asArray(standardPricing == nullptr ? nullptr : *standardPricing); pricing != nullptr &&
      !pricing->empty()) {
    details.unitPrice = readPath((*pricing)[0], {"UnitPrice"}).value_or("");
  } else {
    details.unitPrice = readPath(productNode, {"UnitPrice"}).value_or("");
  }

  details.parameters = extractParameters(productNode);
  const auto combinedText = details.productDescription + " " + details.detailedDescription + " " +
                            details.manufacturerPartNumber;
  if (const auto inductance = extractInductanceFromText(combinedText); inductance.has_value()) {
    upsertExtractedInductance(details.parameters, *inductance);
  }

  if (!details.packageName.empty()) {
    const auto packageExists = any_of(details.parameters.begin(), details.parameters.end(), [](const Parameter& parameter) {
      return toLower(parameter.name) == "package" || toLower(parameter.name) == "package / case" ||
             toLower(parameter.name) == "supplier device package";
    });
    if (!packageExists) {
      details.parameters.push_back({"Package", details.packageName});
    }
  }

  // Keep the catalog response in the common provider shape.  The category ID
  // is optional here because older DigiKey responses do not always expose it.
  details.vendorMetadata.provider = "digikey";
  details.vendorMetadata.providerProductNumber = lookupKey;
  details.vendorMetadata.manufacturerPartNumber = details.manufacturerPartNumber;
  details.vendorMetadata.categoryId = details.categoryName;
  details.vendorMetadata.categoryPath = categoryPath;
  details.vendorMetadata.title = details.productDescription;
  details.vendorMetadata.detailedDescription = details.detailedDescription;
  details.vendorMetadata.parameters = details.parameters;
  details.vendorMetadata.productUrl = details.productUrl;

  return details;
}

}  // namespace digikey_detail
}  // namespace inventatory

#endif
