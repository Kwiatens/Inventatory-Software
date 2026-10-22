// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Mouser catalogue client (Search API v1). A lookup is one keyword search; no second request.
// Field names follow Mouser's published v1 response, not a live one; check them before release.
// Absent fields are absent data, not errors; numbers may arrive as JSON numbers or strings.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "platform/mouser/MouserApi.h"

#include "core/inventory/VendorProvider.h"
#include "platform/digikey/DigiKeyApiPrivate.h"
#include "platform/security/CredentialStore.h"
#include "platform/system/Environment.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <mutex>
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
// Key-test term. Any answer to the search counts, even zero results, because the key was accepted.
constexpr const char* kCredentialTestKeyword = "1N4148";
constexpr size_t kMaximumMouserApiKeyBytes = 256;
// Keywords outside this length cannot be part numbers.
constexpr size_t kMinimumKeywordBytes = 2;
constexpr size_t kMaximumKeywordBytes = 100;
constexpr int kSearchRecords = 50;
constexpr size_t kMaximumMouserProducts = 50;
// 30 calls per minute are allowed; 2.1 s spacing leaves a margin.
constexpr chrono::milliseconds kMinimumRequestSpacing{2100};
// Next free request time, guarded by requestClockMutex. Mouser counts calls per key, not client.
mutex requestClockMutex;
chrono::steady_clock::time_point nextFreeRequestAt{};
// Stock counts and quantities above this are rejected.
constexpr unsigned long long kMaximumCount = 1000000000000ULL;

// A rejected key and a reached limit are reported apart from other failures.
enum class MouserFailure { None, KeyRejected, LimitReached, Other };

bool setError(string* error, string message) {
  if (error != nullptr) *error = move(message);
  return false;
}

// Messages name the vendor as the app shows it, never another vendor.
string mouserName() { return vendorDisplayName(kVendorProviderMouser); }

string missingKeyMessage() { return mouserName() + " API key is missing or not valid"; }
string unreachableMessage() { return "Unable to reach " + mouserName(); }
string unparsableMessage() { return "Unable to parse the " + mouserName() + " response"; }
// mouserLimitReached matches this prefix.
string limitReachedPrefix() { return mouserName() + " request limit reached"; }
string limitReachedMessage() { return limitReachedPrefix() + "; try again later"; }

// mouserKeyRejected matches this prefix.
string keyRejectedPrefix() { return mouserName() + " rejected the API key"; }

string keyRejectedMessage(uint32_t status) {
  return keyRejectedPrefix() + (status >= 400 ? " (HTTP " + to_string(status) + ")" : string());
}

string requestFailedMessage(uint32_t status) {
  return mouserName() + " request failed (HTTP " + to_string(status) + ")";
}

// Text of a string or number; other JSON values give an empty string.
string scalarText(const JsonPtr& value) {
  const auto* node = asValue(value);
  if (node == nullptr) return {};
  if (const auto* text = get_if<string>(&node->data)) return *text;
  if (const auto* number = get_if<JsonValue::Number>(&node->data)) return number->text;
  return {};
}

// Trimmed text of a member; empty if absent or not a string or number.
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

// True when the text is about the key; "keyword" does not count. Some answers say
// "unique identifier" instead.
bool mentionsKey(const string& text) {
  const auto lower = toLower(text);
  for (auto position = lower.find("key"); position != string::npos; position = lower.find("key", position + 1)) {
    if (lower.compare(position, 7, "keyword") != 0) return true;
  }
  return lower.find("unique identifier") != string::npos;
}

// Failure described by the Errors list, or None when it is absent or empty.
MouserFailure readErrors(const JsonPtr& root, string& message) {
  const auto* errors = memberArray(root, "Errors");
  if (errors == nullptr || errors->empty()) return MouserFailure::None;
  bool limit = false;
  bool key = false;
  string first;
  for (const auto& entry : *errors) {
    // Entries are objects with Code, Message and PropertyName; a bare string is its message.
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

// AvailabilityInStock is the count; Availability is a sentence starting with it ("1,234 In Stock").
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

// Lowest-quantity price as Mouser printed it, e.g. "0.05 EUR from 10 pcs". Never converted.
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

// Attributes as name/value pairs; the package is the "Package / Case" attribute.
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

// Parser behind parseMouserSearchResponse; also reports the failure kind.
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

// Reads a keyword-search answer: HTTP status first, then the body.
MouserFailure readAnswer(const HttpResponse& response, vector<MouserProductDetails>& products, string* error) {
  products.clear();
  const auto status = response.statusCode;
  if (status == 429) {
    setError(error, limitReachedMessage());
    return MouserFailure::LimitReached;
  }
  string bodyError;
  const auto failure = readSearchResponse(response.body, products, &bodyError);
  if (status == 401 || status == 403) {
    products.clear();
    // A limit can come with a 401 or 403; the key is not the problem.
    if (failure == MouserFailure::LimitReached) {
      setError(error, limitReachedMessage());
      return failure;
    }
    setError(error, keyRejectedMessage(status));
    return MouserFailure::KeyRejected;
  }
  if (status >= 400) {
    products.clear();
    // Errors on a failing status are reported as key or limit problems when they say so.
    if (failure == MouserFailure::KeyRejected) {
      setError(error, keyRejectedMessage(status));
      return failure;
    }
    if (failure == MouserFailure::LimitReached) {
      setError(error, limitReachedMessage());
      return failure;
    }
    setError(error, requestFailedMessage(status));
    return MouserFailure::Other;
  }
  if (failure != MouserFailure::None) setError(error, move(bodyError));
  return failure;
}

// Sends one search. Only a transport failure is false; the caller reads the status and body.
// The DigiKey-worded transport error is replaced. POSTs are never retried, so a failure costs one
// request.
bool postKeywordSearch(const string& apiKey, const string& keyword, HttpResponse& response, string* error) {
  const auto url = string(kMouserKeywordSearchUrl) + encodeComponent(apiKey, false);
  const auto body = string(R"({"SearchByKeywordRequest":{"keyword":")") + escapeJsonString(keyword) +
                    R"(","records":)" + to_string(kSearchRecords) + R"(,"startingRecord":0}})";
  string transportError;
  if (!requestHttp(L"POST", widen(url), widen(kMouserRequestHeaders), body, response, &transportError,
                   "Mouser")) {
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

bool mouserKeyFromEnvironment() {
  const auto value = environmentValue("MOUSER_API_KEY");
  return value.has_value() && !trim(*value).empty();
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

vector<MouserProductDetails> keepMouserMatches(const vector<MouserProductDetails>& products, const string& partNumber) {
  vector<MouserProductDetails> matches;
  for (const auto& product : products) {
    if (!product.vendorMetadata.providerProductNumber.empty() && mouserPartNumberMatches(partNumber, product)) {
      matches.push_back(product);
    }
  }
  return matches;
}

size_t chooseMouserCandidate(const vector<MouserProductDetails>& matches, const string& /*partNumber*/,
                             const string& manufacturer) {
  // Matches already share the part number, so only the manufacturer can separate them.
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

bool mouserLimitReached(const string& error) { return error.rfind(limitReachedPrefix(), 0) == 0; }

bool mouserKeyRejected(const string& error) { return error.rfind(keyRejectedPrefix(), 0) == 0; }

bool mouserKeyAccepted(uint32_t statusCode, const string& body, string* error) {
  // 429: the key was accepted, but the limit is reached.
  if (statusCode == 429) return true;
  // 1xx and 3xx are not answers to the search.
  if (statusCode < 200 || (statusCode >= 300 && statusCode < 400)) {
    return setError(error, requestFailedMessage(statusCode));
  }
  HttpResponse response;
  response.statusCode = statusCode;
  response.body = body;
  vector<MouserProductDetails> products;
  string answerError;
  // A 2xx passes only with a JSON object body and no Errors; a failing status never passes.
  if (readAnswer(response, products, &answerError) == MouserFailure::None) return true;
  return setError(error, move(answerError));
}

MouserApiClient::MouserApiClient(MouserConfig config) : config_(move(config)) {}

void MouserApiClient::waitForRequestSlot() {
  chrono::steady_clock::time_point slot;
  {
    // Reserve the slot under the lock; sleep after releasing it.
    const lock_guard<mutex> lock(requestClockMutex);
    slot = max(chrono::steady_clock::now(), nextFreeRequestAt);
    nextFreeRequestAt = slot + kMinimumRequestSpacing;
  }
  if (slot > chrono::steady_clock::now()) this_thread::sleep_until(slot);
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
  string answerError;
  // The key passes only if Mouser answers the search; a reached limit also counts.
  if (mouserKeyAccepted(response.statusCode, response.body, &answerError)) return true;
  return setError(error, redactMouserKey(answerError, config_.apiKey));
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
  // Keyword search also returns loose matches; keep only exact part numbers, never guess.
  auto matches = keepMouserMatches(products, keyword);
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
