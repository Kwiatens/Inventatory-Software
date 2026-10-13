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

// One TME catalogue entry. The vendor record keeps the TME symbol as providerProductNumber and the
// manufacturer's part number as manufacturerPartNumber.
struct TmeProductDetails {
  VendorProductMetadata vendorMetadata;
  // Every manufacturer part number TME lists for the product. vendorMetadata holds the first one.
  std::vector<std::string> manufacturerPartNumbers;
  std::string manufacturerName;
  std::string productDescription;
  std::string categoryName;
  // Detail records, read by the product lookup: the parameters in the names the app reads, the package as DigiKey
  // prints it, the datasheet, the product page, the stock and the lowest-quantity price.
  std::vector<Parameter> parameters;
  std::string packageName;
  std::string datasheetUrl;
  std::string productUrl;
  std::string quantityAvailable;
  std::string unitPrice;
  // TME's availability flags for the product, in words ("Hardly available"), and whether it can be ordered at all.
  std::string productStatus;
  bool orderable = true;
};

// The identifier a TME lookup uses: a TME symbol, or a manufacturer part number that is searched and matched
// exactly. An empty value means there is nothing to look up.
struct TmeIdentifier {
  bool isSymbol = false;
  std::string value;
};

// Why a lookup produced no details. NoMatch and Ambiguous are answers from TME; Failed covers transport,
// authorization and parse errors, which a later attempt may resolve.
enum class TmeLookupOutcome {
  Found,
  NoMatch,
  Ambiguous,
  Failed,
};

// Reads the TME credentials and storefront settings: environment variables, then the settings file, then the
// secure credential store, the same order as the DigiKey configuration.
TmeConfig loadTmeConfig();

// Parses a TME product response. The envelope carries status, message and data. A status other than OK or
// SUCCESS fails with the status and message TME sent. A successful response with no products is not an error.
bool parseTmeProductResponse(const std::string& body, std::vector<TmeProductDetails>& products, std::string* error);
// Reads the parameters of one product from a parameters response. The product's symbol selects its element.
bool parseTmeParameterResponse(const std::string& body, const std::string& symbol, TmeProductDetails& product,
                               std::string* error);
// Reads the datasheet (the DTE document) of one product from a files response.
bool parseTmeDocumentResponse(const std::string& body, const std::string& symbol, TmeProductDetails& product,
                              std::string* error);
// Reads the stock and the lowest-quantity price of one product from a data response.
bool parseTmeStockAndPriceResponse(const std::string& body, const std::string& symbol, TmeProductDetails& product,
                                   std::string* error);
// The TME product page for a symbol. The labels' QR codes carry the same address.
std::string tmeProductPageUrl(const std::string& symbol);
// True for the TME statuses that mean the token or the credentials were rejected.
bool isTmeAuthorizationStatus(const std::string& status);
// True when one of the product's manufacturer part numbers equals the given one (case and surrounding spaces
// ignored).
bool tmePartNumberMatches(const std::string& partNumber, const TmeProductDetails& product);

// TME API v2 client. It requests an access token with the token and application secret (OAuth 2.0 client
// credentials, HTTP Basic), then makes bearer-authenticated product requests.
class TmeApiClient {
 public:
  explicit TmeApiClient(TmeConfig config);

  // Checks the credentials: fetches an access token and makes one product request. A rejected token or
  // credential is an error; any other answer from TME means the request was authorized.
  bool testConnection(std::string* error = nullptr);

  // Searches the catalogue by a phrase of 2 to 40 characters. The records carry the symbol and the part number; the
  // caller reads a candidate in full with fetchProductDetails before it relies on the candidate.
  std::vector<TmeProductDetails> searchProducts(const std::string& phrase, TmeLookupOutcome& outcome,
                                                std::string* error = nullptr);
  // Adds the parameters, datasheet, stock and price to a product found by its symbol. Any failed request fails the
  // lookup, so a partly enriched record is never merged.
  bool fetchProductDetails(TmeProductDetails& product, std::string* error);

  std::optional<TmeProductDetails> lookupBySymbol(const std::string& symbol, TmeLookupOutcome& outcome,
                                                  std::string* error = nullptr);
  // Looks the part number up by manufacturer part number, then accepts only a single product whose part numbers
  // include it exactly. Several matches are reported as Ambiguous and change nothing.
  std::optional<TmeProductDetails> lookupByPartNumber(const std::string& partNumber, TmeLookupOutcome& outcome,
                                                      std::string* error = nullptr);

 private:
  // The cached access token, or a new one when there is none or it is about to expire.
  bool accessToken(std::string* error);
  // Requests an access token with the client-credentials grant.
  bool fetchAccessToken(std::string* error);
  // Bearer-authenticated GET of the products endpoint with the given query. The body is returned for the caller to
  // read the response envelope. A rejected access token is replaced once before the request fails.
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
