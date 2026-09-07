#pragma once

#include "core/Inventory.h"

#include <filesystem>
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

 private:
  bool ensureAccessToken(string* error);
  optional<string> requestToken(string* error);
  optional<string> requestProductDetails(const string& productNumber, string* error,
                                                   const string& manufacturerId = "");
  optional<string> requestKeywordSearch(const string& keywords, string* error);

  DigiKeyConfig config_;
  string accessToken_;
  time_t tokenExpiresAt_ = 0;
};

}  // namespace inventatory
