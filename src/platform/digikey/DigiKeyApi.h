#pragma once

#include "core/inventory/Inventory.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

namespace filesystem = std::filesystem;
using std::filesystem::path;
using std::optional;
using std::string;
using std::time_t;
using std::vector;

struct DigiKeyConfig {
  string clientId;
  string clientSecret;
  string accountId;
  string site = "US";
  string language = "en";
  string currency = "USD";

  bool valid() const;
};

struct DigiKeyProductDetails {
  string lookupKey;
  string manufacturerName;
  string manufacturerPartNumber;
  string categoryName;
  string productDescription;
  string detailedDescription;
  string productUrl;
  string datasheetUrl;
  string packagingType;
  string packageName;
  string rohsStatus;
  string leadStatus;
  string productStatus;
  string manufacturerLeadWeeks;
  string quantityAvailable;
  string unitPrice;
  vector<Parameter> parameters;
  VendorProductMetadata vendorMetadata;
};

// Why a product lookup produced no details. NoMatch is an authoritative answer (DigiKey searched
// and found nothing usable); Failed covers transport, authentication, rate limit and parse errors,
// which a later attempt may well resolve.
enum class DigiKeyLookupOutcome {
  Found,
  NoMatch,
  Failed,
};

DigiKeyConfig loadDigiKeyConfig();

// Validates a DigiKey JSON payload without issuing a network request. This
// keeps the bounded parser independently testable for malformed responses.
bool validateDigiKeyJsonPayload(const string& payload, string* error = nullptr);

class DigiKeyApiClient {
 public:
  explicit DigiKeyApiClient(DigiKeyConfig config);

  bool testConnection(string* error = nullptr);

  optional<DigiKeyProductDetails> fetchProductDetails(const string& productNumber,
                                                           string* error = nullptr);

  // Same lookup as fetchProductDetails, additionally telling "nothing matched" from "the request failed".
  optional<DigiKeyProductDetails> lookupProductDetails(const string& productNumber, DigiKeyLookupOutcome& outcome,
                                                       string* error = nullptr);

 private:
  bool ensureAccessToken(string* error);
  bool sendAuthorized(const std::function<optional<string>(const string&, string*)>& buildHeaders,
                      const std::wstring& method, const std::wstring& url, const string& body,
                      std::uint32_t& statusCode, string& responseBody, string* error);
  optional<string> requestToken(string* error);
  optional<string> requestProductDetails(const string& productNumber, string* error,
                                                   const string& manufacturerId = "");
  optional<string> requestKeywordSearch(const string& keywords, string* error);

  DigiKeyConfig config_;
  string accessToken_;
  time_t tokenExpiresAt_ = 0;
};

}  // namespace inventatory
