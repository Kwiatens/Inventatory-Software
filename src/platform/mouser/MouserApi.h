// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Mouser catalogue client (Search API v1, keyword search).

#pragma once

#include "core/inventory/Inventory.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

// Secure credential store name for the Mouser Search API key.
inline constexpr const char* kMouserApiKeyCredentialName = "mouser-api-key";

struct MouserConfig {
  std::string apiKey;
  // True when the key is present, at most 256 bytes, and holds no whitespace or control characters.
  bool valid() const;
};

// Environment variable MOUSER_API_KEY first, then the secure credential store entry above. The key is never read from
// settings.conf.
MouserConfig loadMouserConfig();

// One Mouser catalogue entry, read from one keyword-search result.
struct MouserProductDetails {
  VendorProductMetadata vendorMetadata;  // provider "mouser"; providerProductNumber = the Mouser part number;
                                         // manufacturerPartNumber; categoryPath = {Category} when present; title =
                                         // Description; productUrl = ProductDetailUrl; locale left empty
  std::string manufacturerName;
  std::string productDescription;
  std::string categoryName;
  std::vector<Parameter> parameters;     // ProductAttributes as name/value pairs, values exactly as Mouser sent them
  std::string packageName;               // the value of the attribute named "Package / Case" (case-insensitive), if any
  std::string datasheetUrl;
  std::string productUrl;
  std::string quantityAvailable;         // digits only, e.g. "1234"; empty when unknown
  std::string unitPrice;                 // the lowest-quantity price as Mouser printed it; empty when unknown
  std::string productStatus;             // LifecycleStatus when present, else "Discontinued" when IsDiscontinued is true
  bool orderable = true;                 // false when IsDiscontinued is true or the lifecycle says Obsolete or Discontinued
};

// Why a lookup produced no details. NoMatch and Ambiguous are answers from Mouser; Failed covers transport, key, status
// and parse errors, which a later attempt may resolve.
enum class MouserLookupOutcome { Found, NoMatch, Ambiguous, Failed };

// Parses a keyword-search response body. An envelope with an empty Parts list is success with no products. A non-empty
// Errors list, or a body that is not a JSON object, is a failure with the message set in *error.
bool parseMouserSearchResponse(const std::string& body, std::vector<MouserProductDetails>& products, std::string* error);

// True when the part number equals the product's manufacturer part number OR its Mouser part number. Comparison ignores
// letter case and every character that is not a letter or digit, so "STM32F103C8T6" and "stm32-f103c8t6" are equal.
bool mouserPartNumberMatches(const std::string& partNumber, const MouserProductDetails& product);

// Index of the product a part number means. With a manufacturer given, only products of that manufacturer (case-insensitive)
// are kept. Returns the index of the single remaining product, or matches.size() when none or several remain.
std::size_t chooseMouserCandidate(const std::vector<MouserProductDetails>& matches, const std::string& partNumber,
                                  const std::string& manufacturer);

// Replaces every occurrence of the key in text with "[key]". Used on every error string before it leaves this layer.
std::string redactMouserKey(std::string text, const std::string& apiKey);

// True when an error from this client says Mouser's request limit was reached: HTTP 429, or an Errors entry about a
// limit or quota. Further requests would only spend more of the quota, so a caller stops asking until later.
bool mouserLimitReached(const std::string& error);

// Mouser Search API v1 client. Each request is a POST authenticated by the API key in the query string. The client is
// used from one worker thread at a time, so its request clock needs no lock.
class MouserApiClient {
 public:
  explicit MouserApiClient(MouserConfig config);

  // One keyword search with a fixed term. A rejected key (HTTP 401 or 403, or an Errors entry about the key) is false
  // with an error. Any other answer means the key was accepted.
  bool testConnection(std::string* error = nullptr);

  // Keyword search for the part number, then the exact matches only. Zero matches is NoMatch, and so is a result list
  // with no exact match (never guess from loose matches). Several matches that the manufacturer cannot separate are
  // Ambiguous. Transport, status and parse failures are Failed and carry a message safe to show.
  std::optional<MouserProductDetails> lookupByPartNumber(const std::string& partNumber, MouserLookupOutcome& outcome,
                                                         std::string* error = nullptr,
                                                         const std::string& manufacturer = std::string());

 private:
  bool searchKeyword(const std::string& keyword, std::vector<MouserProductDetails>& products,
                     MouserLookupOutcome& outcome, std::string* error);
  // Sleeps just long enough that calls from this client stay under Mouser's limit of 30 calls per minute.
  void waitForRequestSlot();

  MouserConfig config_;
  std::chrono::steady_clock::time_point lastRequestAt_{};
};

}  // namespace inventatory
