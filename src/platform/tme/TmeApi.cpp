// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - TME catalogue client (API v2).
//
// The v2 flow is the one a community client uses for the current API: a client-credentials token request with the
// token and application secret as HTTP Basic credentials, then bearer-authenticated GET requests. A product lookup
// reads the product record (products), then its parameters, documents, stock and prices, so a merged part carries the
// same kinds of data a DigiKey part does. The response shapes were read from live v2 responses: every list sits under
// data.elements, and each detail record names its symbol. A record that does not match the request is not used.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "platform/tme/TmeApi.h"

#include "app/settings/AppSettings.h"
#include "platform/digikey/DigiKeyApiPrivate.h"
#include "platform/security/CredentialStore.h"
#include "platform/system/Environment.h"
#include "platform/tme/TmeEncoding.h"
#include "platform/tme/TmeVocabulary.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <utility>
#include <variant>

namespace inventatory {
using namespace std;
using digikey_detail::asArray;
using digikey_detail::asObject;
using digikey_detail::asValue;
using digikey_detail::findMember;
using digikey_detail::HttpResponse;
using digikey_detail::JsonPtr;
using digikey_detail::JsonValue;
using digikey_detail::parseJson;
using digikey_detail::readStringPath;
using digikey_detail::requestHttp;
using digikey_detail::widen;
using tme_encoding::base64Encode;
using tme_encoding::percentEncode;

namespace {

constexpr const char* kTmeApiBase = "https://api.tme.eu/";
constexpr const char* kTmeTokenPath = "auth/token";
constexpr const char* kTmeProductsPath = "products";
constexpr const char* kTmeParametersPath = "products/parameters";
constexpr const char* kTmeFilesPath = "products/files";
constexpr const char* kTmeDataPath = "products/data";
constexpr const char* kTmeSearchPath = "products/search";
// Symbol used by the credential test. TME only has to accept the product request; a symbol it does not list still
// counts as a pass, because the token has been accepted.
constexpr const char* kCredentialTestSymbol = "1N4148";
constexpr size_t kMaximumTmeIdentifierBytes = 256;
constexpr size_t kMaximumTmeProducts = 50;
// Used when the token response has no usable expires_in. The token is replaced this many seconds before it ends.
constexpr long long kDefaultTokenLifetimeSeconds = 300;
constexpr long long kTokenRefreshMarginSeconds = 30;

bool setError(string* error, string message) {
  if (error != nullptr) *error = move(message);
  return false;
}

string upperCase(string value) {
  for (auto& character : value) {
    character = static_cast<char>(toupper(static_cast<unsigned char>(character)));
  }
  return value;
}

// The envelope status in upper case: "OK" or "SUCCESS" for a good answer, empty when the body has no status.
string envelopeStatus(const JsonPtr& root) {
  auto status = readStringPath(root, {"status"});
  if (!status.has_value()) status = readStringPath(root, {"Status"});
  return upperCase(trim(status.value_or("")));
}

// Fails when the envelope reports a status other than OK or SUCCESS, with the status and message TME sent.
bool checkEnvelope(const JsonPtr& root, string* error) {
  const auto status = envelopeStatus(root);
  if (status.empty() || status == "OK" || status == "SUCCESS") return true;
  auto message = readStringPath(root, {"message"});
  if (!message.has_value()) message = readStringPath(root, {"ErrorMessage"});
  const auto text = trim(message.value_or(""));
  const string detail = status + (text.empty() ? string() : ": " + text);
  if (isTmeAuthorizationStatus(status)) {
    return setError(error, "TME rejected the token or the application secret (" + detail + ")");
  }
  return setError(error, "TME returned " + detail);
}

// Text of a string or number value. Objects, arrays, booleans and null give nothing.
string scalarText(const JsonPtr& value) {
  const auto* node = asValue(value);
  if (node == nullptr) return {};
  if (const auto* text = get_if<string>(&node->data)) return *text;
  if (const auto* number = get_if<JsonValue::Number>(&node->data)) return number->text;
  return {};
}

// The first non-empty scalar among the keys, trimmed.
string firstText(const JsonPtr& node, initializer_list<const char*> keys) {
  for (const char* key : keys) {
    const auto* member = findMember(node, key);
    if (member == nullptr) continue;
    const auto text = trim(scalarText(*member));
    if (!text.empty()) return text;
  }
  return {};
}

const JsonValue::Array* memberArray(const JsonPtr& object, const char* key) {
  const auto* member = findMember(object, key);
  return member == nullptr ? nullptr : asArray(*member);
}

// The product records of a response. A lookup lists them under data.elements; a search nests them one level
// down, as data.products.elements.
const JsonValue::Array* productRecords(const JsonPtr& root) {
  const auto* data = findMember(root, "data");
  if (data == nullptr) return nullptr;
  if (const auto* list = asArray(*data); list != nullptr) return list;
  if (const auto* list = memberArray(*data, "elements"); list != nullptr) return list;
  if (const auto* products = findMember(*data, "products"); products != nullptr) {
    if (const auto* list = asArray(*products); list != nullptr) return list;
    if (const auto* list = memberArray(*products, "elements"); list != nullptr) return list;
  }
  return memberArray(*data, "ProductList");
}

// The member names of an object, sorted and joined, so an error can say which fields a record has.
string objectKeys(const JsonPtr& node) {
  const auto* object = asObject(node);
  if (object == nullptr) return {};
  vector<string> keys;
  for (const auto& member : *object) keys.push_back(member.first);
  sort(keys.begin(), keys.end());
  string joined;
  for (const auto& key : keys) {
    if (!joined.empty()) joined += ", ";
    joined += key;
  }
  return joined;
}

// The elements of a detail response (parameters, documents, stock and prices), each carrying its symbol.
const JsonValue::Array* detailElements(const JsonPtr& root) {
  const auto* data = findMember(root, "data");
  if (data == nullptr) return nullptr;
  if (const auto* list = asArray(*data); list != nullptr) return list;
  return memberArray(*data, "elements");
}

// The element that carries the symbol. A record for another symbol is never used for this product.
const JsonPtr* elementFor(const JsonValue::Array* elements, const string& symbol) {
  if (elements == nullptr) return nullptr;
  const auto wanted = upperCase(trim(symbol));
  for (const auto& element : *elements) {
    if (upperCase(firstText(element, {"symbol"})) == wanted) return &element;
  }
  return nullptr;
}

// TME's availability flag, in words. The flags are listed by the product record.
string productStatusText(const string& flag) {
  if (flag == "NOT_IN_OFFER") return "Not in offer";
  if (flag == "CANNOT_BE_ORDERED") return "Cannot be ordered";
  if (flag == "BLOCKED_FOR_ZBL_BACKORDERS") return "Blocked for backorders";
  if (flag == "HARDLY_AVAILABLE") return "Hardly available";
  if (flag == "AVAILABLE_WHILE_STOCKS_LAST") return "Available while stocks last";
  return flag;
}

// The text of a parameter's values. TME lists each value with an id; several values are joined.
string parameterValueText(const JsonPtr& parameter) {
  const auto* values = memberArray(parameter, "values");
  if (values == nullptr) return {};
  string joined;
  for (const auto& entry : *values) {
    const auto text = firstText(entry, {"value"});
    if (text.empty()) continue;
    if (!joined.empty()) joined += ", ";
    joined += text;
  }
  return joined;
}

}  // namespace

string tmeProductPageUrl(const string& symbol) {
  return symbol.empty() ? string() : "https://www.tme.eu/details/" + symbol;
}

bool TmeConfig::valid() const {
  return !trim(token).empty() && !trim(applicationSecret).empty() && token.size() <= kMaxTmeFieldBytes &&
         applicationSecret.size() <= kMaxTmeFieldBytes && country.size() <= kMaxTmeLocaleFieldBytes &&
         language.size() <= kMaxTmeLocaleFieldBytes && currency.size() <= kMaxTmeLocaleFieldBytes;
}

TmeConfig loadTmeConfig() {
  TmeConfig config;
  AppSettings settings;
  if (loadAppSettings(appSettingsPath(), settings)) {
    if (!settings.tmeCountry.empty()) config.country = settings.tmeCountry;
    if (!settings.tmeLanguage.empty()) config.language = settings.tmeLanguage;
    if (!settings.tmeCurrency.empty()) config.currency = settings.tmeCurrency;
  }
  if (const auto token = CredentialStore::read(kTmeTokenCredentialName); token.has_value()) {
    config.token = *token;
  }
  if (const auto secret = CredentialStore::read(kTmeSecretCredentialName); secret.has_value()) {
    config.applicationSecret = *secret;
  }
  // Environment variables are applied last, so they override saved values.
  if (const auto value = environmentValue("TME_APP_TOKEN"); value.has_value()) config.token = *value;
  if (const auto value = environmentValue("TME_APP_SECRET"); value.has_value()) config.applicationSecret = *value;
  if (const auto value = environmentValue("TME_COUNTRY"); value.has_value() && !value->empty()) {
    config.country = *value;
  }
  if (const auto value = environmentValue("TME_LANGUAGE"); value.has_value() && !value->empty()) {
    config.language = *value;
  }
  if (const auto value = environmentValue("TME_CURRENCY"); value.has_value() && !value->empty()) {
    config.currency = *value;
  }
  return config;
}

bool isTmeAuthorizationStatus(const string& status) {
  const auto upper = upperCase(status);
  return upper.find("AUTH") != string::npos || upper.find("SIGN") != string::npos ||
         upper.find("TOKEN") != string::npos || upper.find("ACCESS") != string::npos ||
         upper.find("FORBIDDEN") != string::npos;
}

// A part number with its letter case and punctuation removed: the form in which TME and the distributors agree.
string partNumberKey(const string& partNumber) {
  string key;
  for (const unsigned char character : partNumber) {
    if (isalnum(character)) key += static_cast<char>(toupper(character));
  }
  return key;
}

// The manufacturer's name without the company suffixes, in the form the two catalogues agree on ("Littelfuse Inc." and
// "LITTELFUSE" are one maker). Empty for a placeholder such as "Unknown".
string manufacturerKey(const string& manufacturer) {
  static const char* const suffixes[] = {"inc", "incorporated", "corp", "corporation", "co", "ltd", "limited", "llc",
                                         "gmbh", "sa", "plc", "the"};
  string words;
  string word;
  const auto flush = [&]() {
    if (!word.empty() && find(begin(suffixes), end(suffixes), word) == end(suffixes)) words += word + " ";
    word.clear();
  };
  for (const unsigned char character : manufacturer) {
    if (isalnum(character)) {
      word += static_cast<char>(tolower(character));
    } else {
      flush();
    }
  }
  flush();
  if (words.empty() || words == "unknown ") return {};
  return words;
}

bool tmePartNumberMatches(const string& partNumber, const TmeProductDetails& product) {
  const auto wanted = partNumberKey(partNumber);
  if (wanted.empty()) return false;
  const auto equalsWanted = [&](const string& stored) { return partNumberKey(stored) == wanted; };
  if (equalsWanted(product.vendorMetadata.manufacturerPartNumber)) return true;
  return any_of(product.manufacturerPartNumbers.begin(), product.manufacturerPartNumbers.end(), equalsWanted);
}

size_t chooseTmeCandidate(const vector<TmeProductDetails>& matches, const string& partNumber,
                          const string& manufacturer) {
  vector<size_t> candidates;
  for (size_t index = 0; index < matches.size(); ++index) candidates.push_back(index);
  // The part is the one the item's manufacturer makes. When no candidate is from that manufacturer, none can be
  // chosen: the product TME means is not in its list.
  if (const auto wanted = manufacturerKey(manufacturer); !wanted.empty()) {
    vector<size_t> sameMaker;
    for (const auto index : candidates) {
      if (manufacturerKey(matches[index].manufacturerName) == wanted) sameMaker.push_back(index);
    }
    if (sameMaker.empty()) return matches.size();
    candidates = sameMaker;
    if (candidates.size() == 1) return candidates.front();
  }
  // Among one maker's products, the one that carries the part number as its own symbol is the part itself; the others
  // are alternative symbols and packagings of the same MPN. The exact symbol is preferred over one that matches only
  // without punctuation.
  const auto wanted = upperCase(trim(partNumber));
  const auto symbolKey = partNumberKey(partNumber);
  for (const bool exact : {true, false}) {
    vector<size_t> bySymbol;
    for (const auto index : candidates) {
      const auto& symbol = matches[index].vendorMetadata.providerProductNumber;
      if (exact ? upperCase(trim(symbol)) == wanted : partNumberKey(symbol) == symbolKey) bySymbol.push_back(index);
    }
    if (bySymbol.size() == 1) return bySymbol.front();
    if (!bySymbol.empty()) break;
  }
  return matches.size();
}

bool parseTmeProductResponse(const string& body, vector<TmeProductDetails>& products, string* error) {
  products.clear();
  string parseError;
  const auto root = parseJson(body, &parseError);
  if (!root.has_value() || asObject(*root) == nullptr) {
    return setError(error, "Unable to parse the TME response" + (parseError.empty() ? string() : ": " + parseError));
  }
  if (!checkEnvelope(*root, error)) return false;

  const auto* records = productRecords(*root);
  if (records == nullptr) {
    const auto* data = findMember(*root, "data");
    const auto fields = data == nullptr ? string() : objectKeys(*data);
    return setError(error, "The TME response has no product list" +
                               (fields.empty() ? string() : " (data fields: " + fields + ")"));
  }

  string firstRecordFields;
  for (const auto& record : *records) {
    if (products.size() >= kMaximumTmeProducts) break;
    if (asObject(record) == nullptr) continue;
    if (firstRecordFields.empty()) firstRecordFields = objectKeys(record);
    const auto symbol = firstText(record, {"symbol", "Symbol"});
    if (symbol.empty()) continue;

    TmeProductDetails product;
    product.vendorMetadata.provider = "tme";
    product.vendorMetadata.providerProductNumber = symbol;

    // manufacturer_symbols lists the manufacturer part numbers; the original symbol name is the fallback.
    vector<string> partNumbers;
    if (const auto* symbols = memberArray(record, "manufacturer_symbols"); symbols != nullptr) {
      for (const auto& item : *symbols) {
        const auto text = trim(scalarText(item));
        if (!text.empty()) partNumbers.push_back(text);
      }
    }
    if (partNumbers.empty()) {
      const auto original = firstText(record, {"original_symbol", "OriginalSymbol"});
      if (!original.empty()) partNumbers.push_back(original);
    }
    product.manufacturerPartNumbers = partNumbers;
    if (!partNumbers.empty()) product.vendorMetadata.manufacturerPartNumber = partNumbers.front();

    product.manufacturerName = trim(readStringPath(record, {"manufacturer", "name"}).value_or(""));
    if (product.manufacturerName.empty()) {
      product.manufacturerName = firstText(record, {"manufacturer", "producer", "Producer"});
    }
    product.productDescription = firstText(record, {"description", "Description"});
    // TME names the leaf category ("SMD resistors"); the part is classified under the DigiKey family it belongs to.
    auto leafCategory = trim(readStringPath(record, {"category", "name"}).value_or(""));
    if (leafCategory.empty()) leafCategory = firstText(record, {"category", "Category"});
    product.categoryName = tmeCategoryFamily(leafCategory);
    if (product.categoryName.empty()) product.categoryName = leafCategory;
    // The vendor's category path holds the same family DigiKey's does, which the label and rack rules read.
    product.vendorMetadata.categoryPath = product.categoryName.empty() ? vector<string>() : vector<string>{product.categoryName};
    // product_status lists the flags that apply; the part cannot be ordered when TME says so.
    if (const auto* flags = memberArray(record, "product_status"); flags != nullptr) {
      string joined;
      for (const auto& flag : *flags) {
        const auto text = trim(scalarText(flag));
        if (text.empty()) continue;
        if (text == "NOT_IN_OFFER" || text == "CANNOT_BE_ORDERED") product.orderable = false;
        if (!joined.empty()) joined += "; ";
        joined += productStatusText(text);
      }
      product.productStatus = joined;
    }
    if (const auto* category = findMember(record, "category"); category != nullptr) {
      product.vendorMetadata.categoryId = firstText(*category, {"id"});
    }
    product.vendorMetadata.title = product.productDescription;
    products.push_back(move(product));
  }
  if (products.empty() && !firstRecordFields.empty()) {
    return setError(error, "TME product records have no symbol (record fields: " + firstRecordFields + ")");
  }
  return true;
}

bool parseTmeParameterResponse(const string& body, const string& symbol, TmeProductDetails& product, string* error) {
  string parseError;
  const auto root = parseJson(body, &parseError);
  if (!root.has_value() || asObject(*root) == nullptr) {
    return setError(error, "Unable to parse the TME parameters" + (parseError.empty() ? string() : ": " + parseError));
  }
  if (!checkEnvelope(*root, error)) return false;

  product.parameters.clear();
  product.packageName.clear();
  const auto* element = elementFor(detailElements(*root), symbol);
  const auto* container = element == nullptr ? nullptr : findMember(*element, "parameters");
  const auto* entries = container == nullptr ? nullptr : memberArray(*container, "elements");
  if (entries == nullptr) {
    product.vendorMetadata.parameters.clear();
    return true;
  }

  string caseInch;
  string caseMillimetre;
  string caseName;
  for (const auto& entry : *entries) {
    const auto name = firstText(entry, {"name"});
    const auto value = parameterValueText(entry);
    if (name.empty() || value.empty()) continue;
    if (name == "Case - inch") {
      caseInch = value;
    } else if (name == "Case - mm") {
      caseMillimetre = value;
    } else if (name == "Case") {
      caseName = value;
    } else if (name != "Manufacturer") {  // the manufacturer comes from the product record
      for (auto& parameter : tmeParameters(name, value, product.categoryName)) {
        product.parameters.push_back(move(parameter));
      }
    }
  }
  // The package reads as DigiKey prints it: the inch size with the metric size, or the case name when TME gives one.
  string package = caseInch;
  if (!caseInch.empty() && !caseMillimetre.empty()) package += " (" + caseMillimetre + " Metric)";
  // A one-letter case code is TME's internal code (for example "F"), not a package name.
  if (package.empty() && caseName.size() >= 2) package = caseName;
  if (!package.empty()) {
    product.packageName = package;
    product.parameters.push_back({"Package / Case", package});
  }
  product.vendorMetadata.parameters = product.parameters;
  return true;
}

bool parseTmeDocumentResponse(const string& body, const string& symbol, TmeProductDetails& product, string* error) {
  string parseError;
  const auto root = parseJson(body, &parseError);
  if (!root.has_value() || asObject(*root) == nullptr) {
    return setError(error, "Unable to parse the TME documents" + (parseError.empty() ? string() : ": " + parseError));
  }
  if (!checkEnvelope(*root, error)) return false;

  product.datasheetUrl.clear();
  const auto* element = elementFor(detailElements(*root), symbol);
  const auto* container = element == nullptr ? nullptr : findMember(*element, "documents");
  const auto* documents = container == nullptr ? nullptr : memberArray(*container, "elements");
  if (documents == nullptr) return true;
  // The datasheet is the PDF document of type DTE. Other documents are linked files, videos and text.
  for (const auto& document : *documents) {
    if (upperCase(firstText(document, {"type"})) != "DTE") continue;
    auto url = firstText(document, {"url"});
    if (url.rfind("//", 0) == 0) url = "https:" + url;
    const auto lower = toLower(url);
    if (url.rfind("https://www.tme.eu/", 0) != 0 || lower.size() < 4 || lower.substr(lower.size() - 4) != ".pdf") {
      continue;
    }
    product.datasheetUrl = url;
    break;
  }
  return true;
}

bool parseTmeStockAndPriceResponse(const string& body, const string& symbol, TmeProductDetails& product,
                                   string* error) {
  string parseError;
  const auto root = parseJson(body, &parseError);
  if (!root.has_value() || asObject(*root) == nullptr) {
    return setError(error, "Unable to parse the TME stock and prices" +
                               (parseError.empty() ? string() : ": " + parseError));
  }
  if (!checkEnvelope(*root, error)) return false;

  product.quantityAvailable.clear();
  product.unitPrice.clear();
  const auto* element = elementFor(detailElements(*root), symbol);
  if (element == nullptr) return true;
  product.quantityAvailable = firstText(*element, {"stock_quantity"});

  const auto* prices = findMember(*element, "prices");
  const auto* levels = prices == nullptr ? nullptr : memberArray(*prices, "elements");
  if (levels == nullptr) return true;
  // The lowest quantity break is the price a single purchase starts from.
  long long lowestAmount = 0;
  string lowestPrice;
  for (const auto& level : *levels) {
    const auto amountText = firstText(level, {"amount"});
    const auto priceText = firstText(level, {"price"});
    if (amountText.empty() || priceText.empty()) continue;
    char* end = nullptr;
    const long long amount = strtoll(amountText.c_str(), &end, 10);
    if (end == amountText.c_str() || amount <= 0) continue;
    if (lowestAmount == 0 || amount < lowestAmount) {
      lowestAmount = amount;
      lowestPrice = priceText;
    }
  }
  if (lowestAmount == 0) return true;
  const auto currency = trim(firstText(*prices, {"currency"}));
  product.unitPrice = lowestPrice + (currency.empty() ? string() : " " + currency);
  if (lowestAmount > 1) product.unitPrice += " from " + to_string(lowestAmount) + " pcs";
  return true;
}

TmeApiClient::TmeApiClient(TmeConfig config) : config_(move(config)) {}

bool TmeApiClient::fetchAccessToken(string* error) {
  if (!config_.valid()) return setError(error, "TME configuration is incomplete or too large");
  // The token is the Basic user name and the application secret the password, as the v2 token request expects.
  const auto credentials = base64Encode(config_.token + ":" + config_.applicationSecret);
  HttpResponse response;
  if (!requestHttp(L"POST", widen(string(kTmeApiBase) + kTmeTokenPath),
                   widen("Authorization: Basic " + credentials +
                         "\r\nContent-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n"),
                   "grant_type=client_credentials", response, error)) {
    return false;
  }
  if (response.statusCode == 401 || response.statusCode == 403) {
    return setError(error, "TME rejected the token or the application secret (HTTP " +
                               to_string(response.statusCode) + ")");
  }
  string parseError;
  const auto root = parseJson(response.body, &parseError);
  const bool isObject = root.has_value() && asObject(*root) != nullptr;
  if (isObject && !checkEnvelope(*root, error)) return false;
  if (!isObject || response.statusCode >= 400) {
    return setError(error, "TME token request failed (HTTP " + to_string(response.statusCode) + ")");
  }

  // The standard token fields sit at the top level; a data wrapper is accepted as well.
  auto token = firstText(*root, {"access_token"});
  if (token.empty()) token = trim(readStringPath(*root, {"data", "access_token"}).value_or(""));
  if (token.empty()) return setError(error, "TME did not return an access token");

  auto lifetimeText = firstText(*root, {"expires_in"});
  if (lifetimeText.empty()) lifetimeText = trim(readStringPath(*root, {"data", "expires_in"}).value_or(""));
  long long lifetime = kDefaultTokenLifetimeSeconds;
  if (!lifetimeText.empty()) {
    char* end = nullptr;
    const long long parsed = strtoll(lifetimeText.c_str(), &end, 10);
    if (end != lifetimeText.c_str() && parsed > 0) lifetime = parsed;
  }
  accessToken_ = move(token);
  accessTokenExpiresAt_ = chrono::steady_clock::now() +
                          chrono::seconds(max(lifetime - kTokenRefreshMarginSeconds, 0LL));
  return true;
}

bool TmeApiClient::accessToken(string* error) {
  if (!accessToken_.empty() && chrono::steady_clock::now() < accessTokenExpiresAt_) return true;
  accessToken_.clear();
  return fetchAccessToken(error);
}

bool TmeApiClient::getProducts(const vector<pair<string, string>>& query, string& body, string* error) {
  return getEndpoint(kTmeProductsPath, query, body, error);
}

bool TmeApiClient::getEndpoint(const string& path, const vector<pair<string, string>>& query, string& body,
                               string* error) {
  if (!config_.valid()) return setError(error, "TME configuration is incomplete or too large");
  string url = string(kTmeApiBase) + path + "?country=" + percentEncode(config_.country);
  for (const auto& [name, value] : query) {
    url += "&" + percentEncode(name) + "=" + percentEncode(value);
  }
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    if (!accessToken(error)) return false;
    const auto headers = "Authorization: Bearer " + accessToken_ + "\r\nAccept: application/json\r\nAccept-Language: " +
                         config_.language + "\r\n";
    HttpResponse response;
    if (!requestHttp(L"GET", widen(url), widen(headers), "", response, error)) return false;
    if (response.statusCode == 401) {
      // The server no longer accepts the cached token. Fetch a new one and try once more.
      accessToken_.clear();
      continue;
    }
    body = move(response.body);
    return true;
  }
  return setError(error, "TME rejected the access token (HTTP 401)");
}

bool TmeApiClient::fetchProductDetails(TmeProductDetails& product, string* error) {
  const auto symbol = product.vendorMetadata.providerProductNumber;
  string body;
  if (!getEndpoint(kTmeParametersPath, {{"symbols[]", symbol}}, body, error)) return false;
  if (!parseTmeParameterResponse(body, symbol, product, error)) return false;
  if (!getEndpoint(kTmeFilesPath, {{"symbols[]", symbol}}, body, error)) return false;
  if (!parseTmeDocumentResponse(body, symbol, product, error)) return false;
  if (!getEndpoint(kTmeDataPath, {{"symbols[]", symbol}, {"scope[]", "prices"}, {"scope[]", "stock"},
                                  {"currency", config_.currency}},
                   body, error)) {
    return false;
  }
  if (!parseTmeStockAndPriceResponse(body, symbol, product, error)) return false;
  product.productUrl = tmeProductPageUrl(symbol);
  product.vendorMetadata.productUrl = product.productUrl;
  return true;
}

vector<TmeProductDetails> TmeApiClient::searchProducts(const string& phrase, TmeLookupOutcome& outcome,
                                                      string* error) {
  outcome = TmeLookupOutcome::Failed;
  const auto text = trim(phrase);
  // TME takes a phrase of 2 to 40 characters; anything outside that cannot match a part.
  if (text.size() < 2 || text.size() > 40) {
    outcome = TmeLookupOutcome::NoMatch;
    return {};
  }
  string body;
  if (!getEndpoint(kTmeSearchPath, {{"phrase", text}, {"limit", "10"}, {"scope[]", "products"}}, body,
                   error)) {
    return {};
  }
  vector<TmeProductDetails> products;
  if (!parseTmeProductResponse(body, products, error)) return {};
  if (products.empty()) {
    outcome = TmeLookupOutcome::NoMatch;
    return {};
  }
  for (auto& product : products) product.vendorMetadata.locale = config_.language;
  outcome = TmeLookupOutcome::Found;
  return products;
}

bool TmeApiClient::testConnection(string* error) {
  string body;
  if (!getProducts({{"symbols[]", kCredentialTestSymbol}}, body, error)) return false;
  string parseError;
  const auto root = parseJson(body, &parseError);
  if (!root.has_value() || asObject(*root) == nullptr) {
    return setError(error, "Unable to parse the TME response" + (parseError.empty() ? string() : ": " + parseError));
  }
  // Any status other than an authorization failure means TME accepted the token.
  const auto status = envelopeStatus(*root);
  if (isTmeAuthorizationStatus(status)) {
    return setError(error, "TME rejected the token or the application secret (" + status + ")");
  }
  return true;
}

optional<TmeProductDetails> TmeApiClient::lookupBySymbol(const string& symbol, TmeLookupOutcome& outcome,
                                                         string* error) {
  outcome = TmeLookupOutcome::Failed;
  const auto identifier = trim(symbol);
  if (identifier.empty() || identifier.size() > kMaximumTmeIdentifierBytes) {
    setError(error, "TME symbol is empty or too long");
    return nullopt;
  }
  string body;
  if (!getProducts({{"symbols[]", identifier}}, body, error)) return nullopt;
  vector<TmeProductDetails> products;
  if (!parseTmeProductResponse(body, products, error)) return nullopt;
  if (products.empty()) {
    outcome = TmeLookupOutcome::NoMatch;
    setError(error, "TME has no product for symbol " + identifier);
    return nullopt;
  }
  if (products.size() > 1) {
    outcome = TmeLookupOutcome::Ambiguous;
    setError(error, "TME returned several products for symbol " + identifier);
    return nullopt;
  }
  products.front().vendorMetadata.locale = config_.language;
  if (!fetchProductDetails(products.front(), error)) return nullopt;
  outcome = TmeLookupOutcome::Found;
  return move(products.front());
}

optional<TmeProductDetails> TmeApiClient::lookupByPartNumber(const string& partNumber, TmeLookupOutcome& outcome,
                                                             string* error, const string& manufacturer) {
  outcome = TmeLookupOutcome::Failed;
  const auto identifier = trim(partNumber);
  if (identifier.empty() || identifier.size() > kMaximumTmeIdentifierBytes) {
    setError(error, "TME part number is empty or too long");
    return nullopt;
  }
  // Asks TME for the products under a part number, keeping those whose part numbers match it.
  const auto queryMatches = [&](const string& query, vector<TmeProductDetails>& matches) {
    string body;
    if (!getProducts({{"mpns[]", query}}, body, error)) return false;
    vector<TmeProductDetails> products;
    if (!parseTmeProductResponse(body, products, error)) return false;
    for (auto& product : products) {
      if (tmePartNumberMatches(identifier, product)) matches.push_back(move(product));
    }
    return true;
  };

  vector<TmeProductDetails> matches;
  if (!queryMatches(identifier, matches)) return nullopt;
  // TME keeps some part numbers without their punctuation (ERJ6ENF2002V, for ERJ-6ENF2002V), which DigiKey's lookup
  // ignores. The same part is asked for in that form when TME has nothing under the number as written.
  const auto compact = partNumberKey(identifier);
  if (matches.empty() && !compact.empty() && compact != identifier) {
    if (!queryMatches(compact, matches)) return nullopt;
  }
  if (matches.empty()) {
    outcome = TmeLookupOutcome::NoMatch;
    setError(error, "TME has no product with part number " + identifier);
    return nullopt;
  }
  size_t chosen = 0;
  if (matches.size() > 1) {
    chosen = chooseTmeCandidate(matches, identifier, manufacturer);
    if (chosen == matches.size()) {
      outcome = TmeLookupOutcome::Ambiguous;
      setError(error, "TME has several products with part number " + identifier);
      return nullopt;
    }
  }
  TmeProductDetails product = move(matches[chosen]);
  product.vendorMetadata.locale = config_.language;
  if (!fetchProductDetails(product, error)) return nullopt;
  outcome = TmeLookupOutcome::Found;
  return product;
}

}  // namespace inventatory
