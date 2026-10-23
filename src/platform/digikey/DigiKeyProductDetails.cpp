// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - DigiKey product detail extraction.

#include "platform/digikey/DigiKeyApiPrivate.h"

#include "core/parts/PartDescriptor.h"
#include "core/vendor/VendorOffer.h"
#include "platform/system/Environment.h"

#include <algorithm>
#include <regex>
#include <utility>

namespace inventatory {
using namespace std;
namespace digikey_detail {

optional<string> extractComponentPackageFromText(const string& text) {
  if (text.empty()) {
    return nullopt;
  }

  // Compiled once: building a std::regex costs far more than matching.
  static const vector<regex> kPatterns = [] {
    vector<regex> patterns;
    for (const char* pattern : {
             R"(\b(AXIAL|RADIAL|THROUGH HOLE|SURFACE MOUNT|SMD|SMT|MODULE)\b)",
             R"(\b(01005|0201|0402|0603|0805|1206|1210|1812|2010|2512)\b)",
             R"(\b(SOT-?23(?:-?\d+)?)\b)",
             R"(\b(SOT-?223(?:-?\d+)?)\b)",
             R"(\b(SOIC-?\d+)\b)",
             R"(\b(TSSOP-?\d+)\b)",
             R"(\b(SSOP-?\d+)\b)",
             R"(\b(MSOP-?\d+)\b)",
             R"(\b(QFN-?\d+)\b)",
             R"(\b(DFN-?\d+)\b)",
             R"(\b(QFP-?\d+)\b)",
             R"(\b(TQFP-?\d+)\b)",
             R"(\b(LQFP-?\d+)\b)",
             R"(\b(DIP-?\d+)\b)",
             R"(\b(BGA-?\d+)\b)",
             R"(\b(LGA-?\d+)\b)",
             R"(\b(TO-?92(?:-?\d+)?)\b)",
             R"(\b(TO-?220(?:-?\d+)?)\b)",
             R"(\b(TO-?263(?:-?\d+)?)\b)",
         }) {
      patterns.emplace_back(pattern, regex_constants::icase);
    }
    return patterns;
  }();

  // std::regex stack grows with input length, so only the first 4096 bytes are searched.
  // Package names sit in short descriptions.
  constexpr size_t kMaxSearchedBytes = 4096;
  const string searched = text.substr(0, kMaxSearchedBytes);

  for (const auto& re : kPatterns) {
    smatch match;
    if (regex_search(searched, match, re) && match.size() > 1) return match[1].str();
  }

  return nullopt;
}

bool looksLikeInductorText(const string& text) {
  const auto normalized = normalizeParameterKey(text);
  return normalized.find("inductor") != string::npos || normalized.find("fixedind") != string::npos ||
         normalized.find("choke") != string::npos || normalized.find("coil") != string::npos;
}

optional<string> extractInductanceFromText(const string& text) {
  if (!looksLikeInductorText(text)) {
    return nullopt;
  }
  return value_text::extractInductance(text);
}

optional<string> extractComponentPackage(const JsonPtr& product) {
  const auto fromParameter = [&](initializer_list<const char*> names) -> optional<string> {
    // Packaging ("Tape & Reel") can share the "Package" label; skip it and keep looking.
    return readParameterValue(product, names, [](const string& value) { return !looksLikePackagingType(value); });
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
      auto labelText = trim(*label);
      auto valueText = trim(*value);
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
  const auto trimmed = trim(value);
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
    if (package.has_value() && !trim(*package).empty()) {
      return package;
    }
  }
  return nullopt;
}

// A whole count field; nullopt when absent or not digits. DigiKey sends counts as JSON numbers.
optional<uint64_t> readCount(const JsonPtr& node, initializer_list<const char*> path) {
  const auto text = readPath(node, path);
  if (!text.has_value() || !isUnsignedDecimal(*text, 1000000000000ULL)) return nullopt;
  return stoull(trim(*text));
}

// One offer per packaging (cut tape, reel, Digi-Reel). The currency is the one DigiKey says it used,
// which can differ from the one asked for when the site does not sell in it; without it there are
// no offers. Marketplace listings are left out: another seller ships them. A variation with an
// unreadable price is left out whole, so a partial price list never picks the wrong break.
vector<VendorOffer> extractOffers(const JsonPtr& root, const JsonPtr& product, const DigiKeyProductDetails& details) {
  vector<VendorOffer> offers;
  const auto currency = toUpper(trim(readPath(root, {"SearchLocaleUsed", "Currency"}).value_or("")));
  if (!isCurrencyCode(currency)) return offers;
  const auto* variationsNode = findMember(product, "ProductVariations");
  const auto* variations = variationsNode == nullptr ? nullptr : asArray(*variationsNode);
  if (variations == nullptr) return offers;
  const auto status = toLower(details.productStatus);
  const bool orderable = toLower(readPath(product, {"Discontinued"}).value_or("false")) != "true" &&
                         toLower(readPath(product, {"EndOfLife"}).value_or("false")) != "true" &&
                         status.find("obsolete") == string::npos && status.find("discontinued") == string::npos;
  for (const auto& variation : *variations) {
    if (toLower(readPath(variation, {"MarketPlace"}).value_or("false")) == "true") continue;
    const auto sku = trim(readPath(variation, {"DigiKeyProductNumber"}).value_or(""));
    const auto* pricingNode = findMember(variation, "StandardPricing");
    const auto* pricing = pricingNode == nullptr ? nullptr : asArray(*pricingNode);
    if (sku.empty() || pricing == nullptr || pricing->empty()) continue;
    VendorOffer offer;
    bool readable = true;
    for (const auto& entry : *pricing) {
      const auto quantity = readCount(entry, {"BreakQuantity"});
      const auto price = parseDecimalMicros(trim(readPath(entry, {"UnitPrice"}).value_or("")));
      if (!quantity.has_value() || !price.has_value()) {
        readable = false;
        break;
      }
      offer.breaks.push_back({*quantity, *price});
    }
    if (!readable || !normalizePriceBreaks(offer.breaks)) continue;
    offer.provider = "digikey";
    offer.sku = sku;
    offer.manufacturer = details.manufacturerName;
    offer.manufacturerPartNumber = details.manufacturerPartNumber;
    offer.productUrl = vendorLinkAllowed("digikey", details.productUrl) ? details.productUrl : string();
    offer.currency = currency;
    offer.packaging = trim(readPath(variation, {"PackageType", "Name"}).value_or(""));
    offer.stock = readCount(variation, {"QuantityAvailableforPackageType"});
    offer.minimumOrder = max<uint64_t>(readCount(variation, {"MinimumOrderQuantity"}).value_or(1), 1);
    // Tape and reel is sold in whole reels of the standard package.
    if (offer.packaging.find("(TR)") != string::npos) {
      offer.multiple = max<uint64_t>(readCount(variation, {"StandardPackage"}).value_or(1), 1);
    }
    if (const auto fee = parseDecimalMicros(trim(readPath(variation, {"DigiReelFee"}).value_or(""))); fee.has_value() && *fee > 0) {
      offer.reelingFeeMicros = fee;
    }
    offer.lifecycle = details.productStatus;
    offer.orderable = orderable;
    offers.push_back(move(offer));
  }
  return offers;
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

  // Common provider shape. The category ID is optional: older DigiKey responses may omit it.
  details.vendorMetadata.provider = "digikey";
  details.vendorMetadata.providerProductNumber = lookupKey;
  details.vendorMetadata.manufacturerPartNumber = details.manufacturerPartNumber;
  details.vendorMetadata.categoryId = details.categoryName;
  details.vendorMetadata.categoryPath = categoryPath;
  details.vendorMetadata.title = details.productDescription;
  details.vendorMetadata.detailedDescription = details.detailedDescription;
  details.vendorMetadata.parameters = details.parameters;
  details.vendorMetadata.productUrl = details.productUrl;
  details.offers = extractOffers(root, productNode, details);

  return details;
}

bool clearInvalidOptionalFields(DigiKeyProductDetails& details) {
  bool cleared = false;
  const auto dropIfInvalid = [&](string& value, bool valid) {
    if (!value.empty() && !valid) {
      value.clear();
      cleared = true;
    }
  };
  dropIfInvalid(details.quantityAvailable, isUnsignedDecimal(details.quantityAvailable, 1000000000000ULL));
  dropIfInvalid(details.manufacturerLeadWeeks, isUnsignedDecimal(details.manufacturerLeadWeeks, 1000000ULL));
  dropIfInvalid(details.unitPrice, isFiniteDecimal(details.unitPrice, 1000000000000.0));
  return cleared;
}

int tokenLifetimeSeconds(const string& expiresIn) {
  constexpr int kDefaultSeconds = 540;
  constexpr int kMarginSeconds = 60;
  constexpr int kMinimumSeconds = 30;
  constexpr int kMaximumSeconds = 3600;
  if (!isUnsignedDecimal(expiresIn, 86400ULL)) return kDefaultSeconds;
  const auto granted = static_cast<int>(stoull(trim(expiresIn)));
  return clamp(granted - kMarginSeconds, kMinimumSeconds, kMaximumSeconds);
}

}  // namespace digikey_detail
}  // namespace inventatory
