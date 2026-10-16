// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Mouser catalogue client (Search API v1).
//
// A lookup is one keyword search: a POST to search/keyword with the API key in the query string and the keyword in a
// JSON body. The part record (part numbers, manufacturer, stock, price breaks, attributes, lifecycle) comes from that
// one search result, so no second request is made. All knowledge of Mouser's response field names lives in
// parseMouserSearchResponse and the readers it calls. The field names follow Mouser's published v1 search response;
// they have not been read from a live response and must be checked against one before release. An absent field is
// absent data, never an error, and a number may arrive as a JSON number or as a string.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "platform/mouser/MouserApi.h"

#include "core/inventory/VendorProvider.h"
#include "platform/digikey/DigiKeyApiPrivate.h"
#include "platform/security/CredentialStore.h"
#include "platform/system/Environment.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <thread>
#include <utility>
#include <variant>

namespace inventatory {
using namespace std;
using digikey_detail::asArray;
using digikey_detail::asObject;
using digikey_detail::asValue;
using digikey_detail::encodeComponent;
using digikey_detail::escapeJsonString;
using digikey_detail::findMember;
using digikey_detail::HttpResponse;
using digikey_detail::isUnsignedDecimal;
using digikey_detail::JsonPtr;
using digikey_detail::JsonValue;
using digikey_detail::parseJson;
using digikey_detail::requestHttp;
using digikey_detail::widen;

namespace {

constexpr const char* kMouserKeywordSearchUrl = "https://api.mouser.com/api/v1/search/keyword?apiKey=";
constexpr const char* kMouserRequestHeaders = "Content-Type: application/json\r\nAccept: application/json\r\n";
// Term used by the key test. Mouser only has to answer the search; a term with no results still counts as a pass,
// because the key has been accepted.
constexpr const char* kCredentialTestKeyword = "1N4148";
constexpr size_t kMaximumMouserApiKeyBytes = 256;
// Mouser is asked for keywords of 2 to 100 bytes; anything outside that cannot be a part number.
constexpr size_t kMinimumKeywordBytes = 2;
constexpr size_t kMaximumKeywordBytes = 100;
constexpr int kSearchRecords = 50;
constexpr size_t kMaximumMouserProducts = 50;
// Mouser allows 30 calls per minute. 2.1 s between requests keeps one client under that with a margin.
constexpr chrono::milliseconds kMinimumRequestSpacing{2100};
// A stock count or price-break quantity above this is not a real count.
constexpr unsigned long long kMaximumCount = 1000000000000ULL;

// What a failed answer means. The key test tells a rejected key apart from every other failure.
enum class MouserFailure { None, KeyRejected, LimitReached, Other };

bool setError(string* error, string message) {
  if (error != nullptr) *error = move(message);
  return false;
}

// The vendor's name as the app shows it. Every message from this client names Mouser and never another vendor.
string mouserName() { return vendorDisplayName(kVendorProviderMouser); }

string missingKeyMessage() { return mouserName() + " API key is missing or not valid"; }
string unreachableMessage() { return "Unable to reach " + mouserName(); }
string unparsableMessage() { return "Unable to parse the " + mouserName() + " response"; }
string limitReachedMessage() { return mouserName() + " request limit reached; try again later"; }

// The rejected-key message, with the HTTP status when the answer had a failing one.
string keyRejectedMessage(uint32_t status) {
  return mouserName() + " rejected the API key" + (status >= 400 ? " (HTTP " + to_string(status) + ")" : string());
}

// Text of a string or number value. Objects, arrays, booleans and null give nothing.
string scalarText(const JsonPtr& value) {
  const auto* node = asValue(value);
  if (node == nullptr) return {};
  if (const auto* text = get_if<string>(&node->data)) return *text;
  if (const auto* number = get_if<JsonValue::Number>(&node->data)) return number->text;
  return {};
}

// The trimmed text of one member; empty when the member is absent or is not a string or number.
string fieldText(const JsonPtr& node, const char* key) {
  const auto* member = findMember(node, key);
  return member == nullptr ? string() : trim(scalarText(*member));
}

const JsonValue::Array* memberArray(const JsonPtr& object, const char* key) {
  const auto* member = findMember(object, key);
  return member == nullptr ? nullptr : asArray(*member);
}

// True for a JSON true or a string that reads "true" in any letter case.
bool memberIsTrue(const JsonPtr& node, const char* key) {
  const auto* member = findMember(node, key);
  if (member == nullptr) return false;
  if (const auto* value = asValue(*member); value != nullptr) {
    if (const auto* flag = get_if<bool>(&value->data)) return *flag;
  }
  return toLower(trim(scalarText(*member))) == "true";
}

// A part number with its letter case and punctuation removed.
string partNumberKey(const string& partNumber) {
  string key;
  for (const unsigned char character : partNumber) {
    if (isalnum(character)) key += static_cast<char>(toupper(character));
  }
  return key;
}

string withoutSeparators(string text) {
  text.erase(remove_if(text.begin(), text.end(), [](char character) { return character == ',' || character == ' '; }),
             text.end());
  return text;
}

// Error text from Mouser, on one line: control characters become spaces.
string singleLine(string text) {
  for (auto& character : text) {
    if (static_cast<unsigned char>(character) < 0x20U || character == '\x7f') character = ' ';
  }
  return trim(text);
}

bool mentionsLimit(const string& text) {
  const auto lower = toLower(text);
  for (const char* word : {"limit", "too many", "toomany", "quota", "per minute", "per hour", "per day"}) {
    if (lower.find(word) != string::npos) return true;
  }
  return false;
}

// True when the text is about the API key. "keyword" is a field of the request, not the key. Mouser calls the key a
// unique identifier in some answers.
bool mentionsKey(const string& text) {
  const auto lower = toLower(text);
  for (auto position = lower.find("key"); position != string::npos; position = lower.find("key", position + 1)) {
    if (lower.compare(position, 7, "keyword") != 0) return true;
  }
  return lower.find("unique identifier") != string::npos;
}

// Reads the Errors list. None when it is absent or empty; otherwise the failure it describes, with its message.
MouserFailure readErrors(const JsonPtr& root, string& message) {
  const auto* errors = memberArray(root, "Errors");
  if (errors == nullptr || errors->empty()) return MouserFailure::None;
  bool limit = false;
  bool key = false;
  string first;
  for (const auto& entry : *errors) {
    // An entry is an object with Code, Message and PropertyName; a bare string is read as its message.
    const auto text = asObject(entry) != nullptr ? fieldText(entry, "Message") : trim(scalarText(entry));
    const auto code = fieldText(entry, "Code");
    const auto property = fieldText(entry, "PropertyName");
    if (first.empty()) first = singleLine(text.empty() ? code : text);
    for (const auto* part : {&text, &code, &property}) {
      limit = limit || mentionsLimit(*part);
      key = key || mentionsKey(*part);
    }
  }
  if (limit) {
    message = limitReachedMessage();
    return MouserFailure::LimitReached;
  }
  if (key) {
    message = keyRejectedMessage(0);
    return MouserFailure::KeyRejected;
  }
  message = first.empty() ? mouserName() + " returned an error" : mouserName() + ": " + first;
  return MouserFailure::Other;
}

// The stock count as digits. AvailabilityInStock is the count itself; Availability is a sentence that starts with it
// ("1,234 In Stock").
string stockCount(const JsonPtr& record) {
  const auto inStock = withoutSeparators(fieldText(record, "AvailabilityInStock"));
  if (isUnsignedDecimal(inStock, kMaximumCount)) return inStock;
  const auto availability = fieldText(record, "Availability");
  if (availability.empty() || !isdigit(static_cast<unsigned char>(availability.front()))) return {};
  string digits;
  size_t index = 0;
  for (; index < availability.size(); ++index) {
    const auto character = static_cast<unsigned char>(availability[index]);
    if (isdigit(character)) {
      digits += static_cast<char>(character);
    } else if (character != ',') {
      break;
    }
  }
  // The number must stand on its own: "12.5k Available" is not a count.
  if (index < availability.size() && !isspace(static_cast<unsigned char>(availability[index]))) return {};
  return isUnsignedDecimal(digits, kMaximumCount) ? digits : string();
}

// The price of the smallest quantity break that has a price, as Mouser printed it, with its currency and the quantity
// it applies from: "$0.10 USD", "0.05 EUR from 10 pcs". The number is never converted.
string lowestQuantityPrice(const JsonPtr& record) {
  const auto* breaks = memberArray(record, "PriceBreaks");
  if (breaks == nullptr) return {};
  const JsonPtr* lowest = nullptr;
  unsigned long long lowestQuantity = 0;
  for (const auto& entry : *breaks) {
    const auto quantityText = withoutSeparators(fieldText(entry, "Quantity"));
    if (fieldText(entry, "Price").empty() || !isUnsignedDecimal(quantityText, kMaximumCount)) continue;
    const auto quantity = stoull(quantityText);
    if (quantity == 0) continue;
    if (lowest == nullptr || quantity < lowestQuantity) {
      lowest = &entry;
      lowestQuantity = quantity;
    }
  }
  if (lowest == nullptr) return {};
  auto price = fieldText(*lowest, "Price");
  const auto currency = fieldText(*lowest, "Currency");
  if (!currency.empty() && !containsInsensitive(price, currency)) price += " " + currency;
  if (lowestQuantity > 1) price += " from " + to_string(lowestQuantity) + " pcs";
  return price;
}

// The attributes as name/value pairs, exactly as Mouser sent them. The package is the "Package / Case" attribute.
void readAttributes(const JsonPtr& record, MouserProductDetails& product) {
  const auto* attributes = memberArray(record, "ProductAttributes");
  if (attributes == nullptr) return;
  for (const auto& entry : *attributes) {
    const auto name = fieldText(entry, "AttributeName");
    if (name.empty()) continue;
    const auto value = fieldText(entry, "AttributeValue");
    if (product.packageName.empty() && toLower(name) == "package / case") product.packageName = value;
    product.parameters.push_back({name, value});
  }
  product.vendorMetadata.parameters = product.parameters;
}

// The lifecycle in words, and whether the part can still be ordered.
void readLifecycle(const JsonPtr& record, MouserProductDetails& product) {
  const auto lifecycle = fieldText(record, "LifecycleStatus");
  const bool discontinued = memberIsTrue(record, "IsDiscontinued");
  if (!lifecycle.empty()) {
    product.productStatus = lifecycle;
  } else if (discontinued) {
    product.productStatus = "Discontinued";
  }
  if (discontinued || containsInsensitive(lifecycle, "obsolete") || containsInsensitive(lifecycle, "discontinued")) {
    product.orderable = false;
  }
}

MouserProductDetails readPart(const JsonPtr& record) {
  MouserProductDetails product;
  product.vendorMetadata.provider = kVendorProviderMouser;
  auto mouserNumber = fieldText(record, "MouserPartNumber");
  // A placeholder such as "N/A" is not a Mouser part number.
  if (toUpper(mouserNumber) == "N/A") mouserNumber.clear();
  product.vendorMetadata.providerProductNumber = mouserNumber;
  product.vendorMetadata.manufacturerPartNumber = fieldText(record, "ManufacturerPartNumber");
  product.manufacturerName = fieldText(record, "Manufacturer");
  product.productDescription = fieldText(record, "Description");
  product.categoryName = fieldText(record, "Category");
  if (!product.categoryName.empty()) product.vendorMetadata.categoryPath = {product.categoryName};
  product.vendorMetadata.title = product.productDescription;
  product.datasheetUrl = fieldText(record, "DataSheetUrl");
  product.productUrl = fieldText(record, "ProductDetailUrl");
  product.vendorMetadata.productUrl = product.productUrl;
  product.quantityAvailable = stockCount(record);
  product.unitPrice = lowestQuantityPrice(record);
  readAttributes(record, product);
  readLifecycle(record, product);
  return product;
}

// The parser behind parseMouserSearchResponse. It also says what kind of failure the body reports, so a caller can
// tell a rejected key or a reached limit from other failures.
MouserFailure readSearchResponse(const string& body, vector<MouserProductDetails>& products, string* error) {
  products.clear();
  string parseError;
  const auto root = parseJson(body, &parseError);
  if (!root.has_value() || asObject(*root) == nullptr) {
    setError(error, unparsableMessage());
    return MouserFailure::Other;
  }
  string message;
  if (const auto failure = readErrors(*root, message); failure != MouserFailure::None) {
    setError(error, message);
    return failure;
  }
  const auto* results = findMember(*root, "SearchResults");
  const auto* parts = results == nullptr ? nullptr : memberArray(*results, "Parts");
  if (parts == nullptr) return MouserFailure::None;
  for (const auto& record : *parts) {
    if (products.size() >= kMaximumMouserProducts) break;
    if (asObject(record) == nullptr) continue;
    auto product = readPart(record);
    // A record without either part number cannot be matched to anything.
    if (product.vendorMetadata.providerProductNumber.empty() && product.vendorMetadata.manufacturerPartNumber.empty()) {
      continue;
    }
    products.push_back(move(product));
  }
  return MouserFailure::None;
}

// Reads one keyword-search answer: the HTTP status first, then the body.
MouserFailure readAnswer(const HttpResponse& response, vector<MouserProductDetails>& products, string* error) {
  products.clear();
  const auto status = response.statusCode;
  if (status == 401 || status == 403) {
    setError(error, keyRejectedMessage(status));
    return MouserFailure::KeyRejected;
  }
  if (status == 429) {
    setError(error, limitReachedMessage());
    return MouserFailure::LimitReached;
  }
  string bodyError;
  const auto failure = readSearchResponse(response.body, products, &bodyError);
  if (status >= 400) {
    products.clear();
    // A failing status may still carry an Errors list. One about the key or the limit is reported as such.
    if (failure == MouserFailure::KeyRejected) {
      setError(error, keyRejectedMessage(status));
      return failure;
    }
    if (failure == MouserFailure::LimitReached) {
      setError(error, limitReachedMessage());
      return failure;
    }
    setError(error, mouserName() + " request failed (HTTP " + to_string(status) + ")");
    return MouserFailure::Other;
  }
  if (failure != MouserFailure::None) setError(error, move(bodyError));
  return failure;
}

// Sends one keyword search. Only a transport failure is false; the caller reads the status and the body. The shared
// transport words its own errors for DigiKey, so they are replaced: a Mouser error never names another vendor. A POST
// is never repeated by the transport, so a failed call costs at most one request of the quota.
bool postKeywordSearch(const string& apiKey, const string& keyword, HttpResponse& response, string* error) {
  const auto url = string(kMouserKeywordSearchUrl) + encodeComponent(apiKey, false);
  const auto body = string(R"({"SearchByKeywordRequest":{"keyword":")") + escapeJsonString(keyword) +
                    R"(","records":)" + to_string(kSearchRecords) + R"(,"startingRecord":0}})";
  string transportError;
  if (!requestHttp(L"POST", widen(url), widen(kMouserRequestHeaders), body, response, &transportError)) {
    return setError(error, unreachableMessage());
  }
  return true;
}

}  // namespace

bool MouserConfig::valid() const {
  if (apiKey.empty() || apiKey.size() > kMaximumMouserApiKeyBytes) return false;
  return none_of(apiKey.begin(), apiKey.end(), [](unsigned char character) {
    return character <= 0x20U || character == 0x7fU;
  });
}

MouserConfig loadMouserConfig() {
  MouserConfig config;
  // The environment variable wins; the credential store is read only when it is unset or blank.
  if (const auto value = environmentValue("MOUSER_API_KEY"); value.has_value() && !trim(*value).empty()) {
    config.apiKey = trim(*value);
    return config;
  }
  if (const auto stored = CredentialStore::read(kMouserApiKeyCredentialName); stored.has_value()) {
    config.apiKey = trim(*stored);
  }
  return config;
}

bool parseMouserSearchResponse(const string& body, vector<MouserProductDetails>& products, string* error) {
  return readSearchResponse(body, products, error) == MouserFailure::None;
}

bool mouserPartNumberMatches(const string& partNumber, const MouserProductDetails& product) {
  const auto wanted = partNumberKey(partNumber);
  if (wanted.empty()) return false;
  return partNumberKey(product.vendorMetadata.manufacturerPartNumber) == wanted ||
         partNumberKey(product.vendorMetadata.providerProductNumber) == wanted;
}

size_t chooseMouserCandidate(const vector<MouserProductDetails>& matches, const string& /*partNumber*/,
                             const string& manufacturer) {
  // The matches are the exact matches for the part number already, so only the manufacturer can tell them apart.
  const auto wanted = toLower(trim(manufacturer));
  size_t chosen = matches.size();
  size_t remaining = 0;
  for (size_t index = 0; index < matches.size(); ++index) {
    if (!wanted.empty() && toLower(trim(matches[index].manufacturerName)) != wanted) continue;
    chosen = index;
    ++remaining;
  }
  return remaining == 1 ? chosen : matches.size();
}

string redactMouserKey(string text, const string& apiKey) {
  if (apiKey.empty()) return text;
  const string replacement = "[key]";
  for (auto position = text.find(apiKey); position != string::npos;
       position = text.find(apiKey, position + replacement.size())) {
    text.replace(position, apiKey.size(), replacement);
  }
  return text;
}

MouserApiClient::MouserApiClient(MouserConfig config) : config_(move(config)) {}

void MouserApiClient::waitForRequestSlot() {
  if (lastRequestAt_ != chrono::steady_clock::time_point{}) {
    const auto readyAt = lastRequestAt_ + kMinimumRequestSpacing;
    const auto now = chrono::steady_clock::now();
    if (now < readyAt) this_thread::sleep_for(readyAt - now);
  }
  lastRequestAt_ = chrono::steady_clock::now();
}

bool MouserApiClient::searchKeyword(const string& keyword, vector<MouserProductDetails>& products,
                                    MouserLookupOutcome& outcome, string* error) {
  products.clear();
  outcome = MouserLookupOutcome::Failed;
  if (!config_.valid()) return setError(error, missingKeyMessage());
  waitForRequestSlot();
  HttpResponse response;
  if (!postKeywordSearch(config_.apiKey, keyword, response, error)) return false;
  string answerError;
  if (readAnswer(response, products, &answerError) != MouserFailure::None) {
    return setError(error, redactMouserKey(answerError, config_.apiKey));
  }
  outcome = products.empty() ? MouserLookupOutcome::NoMatch : MouserLookupOutcome::Found;
  return true;
}

bool MouserApiClient::testConnection(string* error) {
  if (!config_.valid()) return setError(error, missingKeyMessage());
  waitForRequestSlot();
  HttpResponse response;
  if (!postKeywordSearch(config_.apiKey, kCredentialTestKeyword, response, error)) return false;
  vector<MouserProductDetails> products;
  string answerError;
  // Only a rejected key fails the test. Any other answer, a reached limit or a failed search included, means Mouser
  // accepted the key.
  if (readAnswer(response, products, &answerError) == MouserFailure::KeyRejected) {
    return setError(error, redactMouserKey(answerError, config_.apiKey));
  }
  return true;
}

optional<MouserProductDetails> MouserApiClient::lookupByPartNumber(const string& partNumber,
                                                                   MouserLookupOutcome& outcome, string* error,
                                                                   const string& manufacturer) {
  outcome = MouserLookupOutcome::Failed;
  const auto keyword = trim(partNumber);
  if (keyword.size() < kMinimumKeywordBytes || keyword.size() > kMaximumKeywordBytes) {
    outcome = MouserLookupOutcome::NoMatch;
    setError(error, mouserName() + " has no product with that part number");
    return nullopt;
  }
  vector<MouserProductDetails> products;
  if (!searchKeyword(keyword, products, outcome, error)) return nullopt;
  // A keyword search also lists loose matches (longer part numbers, parts that mention the term). Only the products
  // whose part number is the one asked for are kept; a part is never guessed from the others.
  vector<MouserProductDetails> matches;
  for (auto& product : products) {
    if (mouserPartNumberMatches(keyword, product)) matches.push_back(move(product));
  }
  if (matches.empty()) {
    outcome = MouserLookupOutcome::NoMatch;
    setError(error, redactMouserKey(mouserName() + " has no product with part number " + keyword, config_.apiKey));
    return nullopt;
  }
  size_t chosen = 0;
  if (matches.size() > 1) {
    chosen = chooseMouserCandidate(matches, keyword, manufacturer);
    if (chosen == matches.size()) {
      outcome = MouserLookupOutcome::Ambiguous;
      setError(error,
               redactMouserKey(mouserName() + " has several products with part number " + keyword, config_.apiKey));
      return nullopt;
    }
  }
  outcome = MouserLookupOutcome::Found;
  return move(matches[chosen]);
}

}  // namespace inventatory
