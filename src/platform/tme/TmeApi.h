// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "core/inventory/Inventory.h"

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace inventatory {

// Secure credential store names for the TME token and application secret.
inline constexpr const char* kTmeTokenCredentialName = "tme-app-token";
inline constexpr const char* kTmeSecretCredentialName = "tme-app-secret";

struct TmeConfig {
  std::string token;
  std::string applicationSecret;
  std::string country = "PL";
  std::string language = "EN";
  std::string currency = "EUR";

  bool valid() const;
};

// One TME catalogue entry. vendorMetadata keeps the TME symbol and the part number.
struct TmeProductDetails {
  VendorProductMetadata vendorMetadata;
  // Every manufacturer part number TME lists for the product. vendorMetadata holds the first one.
  std::vector<std::string> manufacturerPartNumbers;
  std::string manufacturerName;
  std::string productDescription;
  std::string categoryName;
  // Filled by the detail lookups. packageName uses DigiKey's wording.
  std::vector<Parameter> parameters;
  std::string packageName;
  std::string datasheetUrl;
  std::string productUrl;
  std::string quantityAvailable;
  std::string unitPrice;
  // TME's availability status in words, and whether the product can be ordered.
  std::string productStatus;
  bool orderable = true;
};

// What a TME lookup searches for: a symbol, or a part number matched exactly.
// An empty value means no lookup. The manufacturer tells apart products that share a part number.
struct TmeIdentifier {
  bool isSymbol = false;
  std::string value;
  std::string manufacturer;
};

// NoMatch and Ambiguous are answers from TME. Failed (transport, auth, parse) may succeed on retry.
enum class TmeLookupOutcome {
  Found,
  NoMatch,
  Ambiguous,
  Failed,
};

// Reads credentials and storefront settings: environment variables, the settings file, then the
// credential store, in the same order as DigiKey.
TmeConfig loadTmeConfig();

// Parses a TME product response. A status other than OK or SUCCESS fails with TME's status
// and message. No products is not an error.
bool parseTmeProductResponse(const std::string& body, std::vector<TmeProductDetails>& products, std::string* error);
bool parseTmeParameterResponse(const std::string& body, const std::string& symbol, TmeProductDetails& product,
                               std::string* error);
// Reads the datasheet (the DTE document) of one product from a files response.
bool parseTmeDocumentResponse(const std::string& body, const std::string& symbol, TmeProductDetails& product,
                              std::string* error);
bool parseTmeStockAndPriceResponse(const std::string& body, const std::string& symbol, TmeProductDetails& product,
                                   std::string* error);
// The TME product page for a symbol. The labels' QR codes carry the same address.
std::string tmeProductPageUrl(const std::string& symbol);
// True for the TME statuses that mean the token or the credentials were rejected.
bool isTmeAuthorizationStatus(const std::string& status);
// True when a manufacturer part number of the product matches, ignoring case and punctuation.
bool tmePartNumberMatches(const std::string& partNumber, const TmeProductDetails& product);
// Picks the product for a part number TME lists several times. The manufacturer, when given,
// filters first, then the product whose symbol is the part number. Returns matches.size() if ambiguous.
std::size_t chooseTmeCandidate(const std::vector<TmeProductDetails>& matches, const std::string& partNumber,
                               const std::string& manufacturer);

// TME API v2 client. It gets an access token with OAuth 2.0 client credentials (HTTP Basic), then
// makes bearer-authenticated requests.
class TmeApiClient {
 public:
  explicit TmeApiClient(TmeConfig config);

  // Fetches a token and makes one product request. A rejected credential is an error; any other
  // answer means it was authorized.
  bool testConnection(std::string* error = nullptr);

  // Searches by a phrase of 2 to 40 characters. Results carry the symbol and part number; call
  // fetchProductDetails before relying on one.
  std::vector<TmeProductDetails> searchProducts(const std::string& phrase, TmeLookupOutcome& outcome,
                                                std::string* error = nullptr);
  // Adds parameters, datasheet, stock and price. Any failed request fails the lookup, so a partial
  // record is never merged.
  bool fetchProductDetails(TmeProductDetails& product, std::string* error);

  std::optional<TmeProductDetails> lookupBySymbol(const std::string& symbol, TmeLookupOutcome& outcome,
                                                  std::string* error = nullptr);
  // Accepts the single product whose part numbers include it, or the one chooseTmeCandidate picks.
  // Still ambiguous is reported as Ambiguous and changes nothing.
  std::optional<TmeProductDetails> lookupByPartNumber(const std::string& partNumber, TmeLookupOutcome& outcome,
                                                      std::string* error = nullptr,
                                                      const std::string& manufacturer = std::string());

 private:
  // The cached access token, or a new one when there is none or it is about to expire.
  bool accessToken(std::string* error);
  bool fetchAccessToken(std::string* error);
  // Bearer GET of the products endpoint; the caller parses the envelope in body.
  // A rejected token is replaced once before the request fails.
  bool getProducts(const std::vector<std::pair<std::string, std::string>>& query, std::string& body,
                   std::string* error);
  bool getEndpoint(const std::string& path, const std::vector<std::pair<std::string, std::string>>& query,
                   std::string& body, std::string* error);
  TmeConfig config_;
  // The client is used from one worker thread at a time, so the cached token needs no lock.
  std::string accessToken_;
  std::chrono::steady_clock::time_point accessTokenExpiresAt_{};
};

}  // namespace inventatory
