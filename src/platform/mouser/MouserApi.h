// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/inventory/Inventory.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

inline constexpr const char* kMouserApiKeyCredentialName = "mouser-api-key";

struct MouserConfig {
  std::string apiKey;
  // True when the key is present, at most 256 bytes, and holds no whitespace or control characters.
  bool valid() const;
};

// Reads MOUSER_API_KEY first, then the credential store. Never read from settings.conf.
MouserConfig loadMouserConfig();
// True when MOUSER_API_KEY is non-blank. It replaces the stored key, so Settings counts it as set.
bool mouserKeyFromEnvironment();

// One Mouser catalogue entry, read from one keyword-search result.
struct MouserProductDetails {
  // provider "mouser"; providerProductNumber = Mouser part number; title = Description.
  VendorProductMetadata vendorMetadata;
  std::string manufacturerName;
  std::string productDescription;
  std::string categoryName;
  std::vector<Parameter> parameters;     // Name/value pairs, values as Mouser sent them
  std::string packageName;               // From the "Package / Case" attribute (case-insensitive)
  std::string datasheetUrl;
  std::string productUrl;
  std::string quantityAvailable;         // Digits only; empty when unknown
  std::string unitPrice;                 // Lowest-quantity price, as Mouser printed it
  std::string productStatus;             // LifecycleStatus, else "Discontinued" when IsDiscontinued
  bool orderable = true;                 // False when discontinued or obsolete
};

// NoMatch and Ambiguous are Mouser's answers. Failed (transport, key, status, parse) may succeed
// later.
enum class MouserLookupOutcome { Found, NoMatch, Ambiguous, Failed };

// An empty Parts list is success with no products. A non-empty Errors list or a non-object body is
// a failure; the message goes to *error.
bool parseMouserSearchResponse(const std::string& body, std::vector<MouserProductDetails>& products, std::string* error);

// Matches either part number, ignoring case and non-alphanumeric characters.
bool mouserPartNumberMatches(const std::string& partNumber, const MouserProductDetails& product);

// Matches that also carry a Mouser part number; without one the record cannot be ordered or merged.
std::vector<MouserProductDetails> keepMouserMatches(const std::vector<MouserProductDetails>& products,
                                                    const std::string& partNumber);

// Filters by manufacturer (case-insensitive) when given. Returns the index of the sole remaining
// product, or matches.size() if none or several remain.
std::size_t chooseMouserCandidate(const std::vector<MouserProductDetails>& matches, const std::string& partNumber,
                                  const std::string& manufacturer);

// Replaces the key with "[key]". Applied to every error string before it leaves this layer.
std::string redactMouserKey(std::string text, const std::string& apiKey);

// True for HTTP 429 or a limit/quota error. Callers should stop asking until later.
bool mouserLimitReached(const std::string& error);

// True for HTTP 401/403 or a key error. Callers should stop until the key changes.
bool mouserKeyRejected(const std::string& error);

// Whether a key test shows Mouser accepted the key. HTTP 429 counts as accepted. A 2xx counts only
// with a JSON object body and no Errors. Otherwise false, with the reason in *error.
bool mouserKeyAccepted(std::uint32_t statusCode, const std::string& body, std::string* error);

// Mouser Search API v1 client. Requests are spaced process-wide to respect the key's rate limit.
class MouserApiClient {
 public:
  explicit MouserApiClient(MouserConfig config);

  // Keyword search with a fixed term; see mouserKeyAccepted for the result rules.
  bool testConnection(std::string* error = nullptr);

  // Keyword search, then exact matches only; never guess from loose matches. Several matches that
  // the manufacturer cannot separate are Ambiguous.
  std::optional<MouserProductDetails> lookupByPartNumber(const std::string& partNumber, MouserLookupOutcome& outcome,
                                                         std::string* error = nullptr,
                                                         const std::string& manufacturer = std::string());

 private:
  bool searchKeyword(const std::string& keyword, std::vector<MouserProductDetails>& products,
                     MouserLookupOutcome& outcome, std::string* error);
  // Sleeps so all clients together stay under Mouser's limit of 30 calls per minute.
  void waitForRequestSlot();

  MouserConfig config_;
};

}  // namespace inventatory
