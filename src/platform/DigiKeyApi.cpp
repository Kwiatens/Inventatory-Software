// Inventatory - DigiKey client and configuration workflow.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "platform/DigiKeyApiPrivate.h"

#include "app/AppSettings.h"
#include "platform/CredentialStore.h"
#include "platform/Environment.h"

#ifdef _WIN32

#include <ctime>
#include <utility>

namespace inventatory {
using namespace std;
using namespace digikey_detail;

bool DigiKeyConfig::valid() const {
  return !trimCopy(clientId).empty() && !trimCopy(clientSecret).empty() &&
         clientId.size() <= kMaximumDigiKeyFieldBytes && clientSecret.size() <= kMaximumDigiKeyFieldBytes &&
         accountId.size() <= kMaximumDigiKeyFieldBytes && site.size() <= kMaximumDigiKeyFieldBytes &&
         language.size() <= kMaximumDigiKeyFieldBytes && currency.size() <= kMaximumDigiKeyFieldBytes &&
         isSafeHeaderValue(clientId) && isSafeHeaderValue(accountId) && isSafeHeaderValue(site) &&
         isSafeHeaderValue(language) && isSafeHeaderValue(currency);
}

bool validateDigiKeyJsonPayload(const string& payload, string* error) {
  if (error != nullptr) error->clear();
  string parseError;
  if (!parseJson(payload, &parseError).has_value()) {
    if (error != nullptr) *error = move(parseError);
    return false;
  }
  return true;
}

DigiKeyConfig loadDigiKeyConfig() {
  DigiKeyConfig config;
  if (const auto value = environmentValue("DIGIKEY_CLIENT_ID"); value.has_value()) {
    config.clientId = *value;
  }
  if (const auto value = environmentValue("DIGIKEY_ACCOUNT_ID"); value.has_value()) {
    config.accountId = *value;
  }
  if (const auto value = environmentValue("DIGIKEY_SITE"); value.has_value() && !value->empty()) {
    config.site = *value;
  }
  if (const auto value = environmentValue("DIGIKEY_LANGUAGE"); value.has_value() && !value->empty()) {
    config.language = *value;
  }
  if (const auto value = environmentValue("DIGIKEY_CURRENCY"); value.has_value() && !value->empty()) {
    config.currency = *value;
  }
  AppSettings settings;
  if (loadAppSettings(appSettingsPath(), settings)) {
    if (!settings.digiKeyClientId.empty()) config.clientId = settings.digiKeyClientId;
    if (!settings.digiKeyAccountId.empty()) config.accountId = settings.digiKeyAccountId;
    if (!settings.digiKeySite.empty()) config.site = settings.digiKeySite;
    if (!settings.digiKeyLanguage.empty()) config.language = settings.digiKeyLanguage;
    if (!settings.digiKeyCurrency.empty()) config.currency = settings.digiKeyCurrency;
  }
  if (const auto secret = CredentialStore::read("digikey-client-secret"); secret.has_value()) {
    config.clientSecret = *secret;
  }
  return config;
}

DigiKeyApiClient::DigiKeyApiClient(DigiKeyConfig config) : config_(move(config)) {}

bool DigiKeyApiClient::testConnection(string* error) {
  return ensureAccessToken(error);
}

optional<string> DigiKeyApiClient::requestToken(string* error) {
  if (!config_.valid()) {
    if (error != nullptr) *error = "DigiKey configuration is incomplete or too large";
    return nullopt;
  }
  const wstring url = L"https://api.digikey.com/v1/oauth2/token";
  ostringstream body;
  body << "client_id=" << encodeFormValue(config_.clientId) << "&client_secret=" << encodeFormValue(config_.clientSecret)
       << "&grant_type=client_credentials";

  HttpResponse response;
  if (!requestHttp(L"POST", url, L"Content-Type: application/x-www-form-urlencoded\r\n", body.str(), response, error)) {
    return nullopt;
  }
  if (response.statusCode < 200 || response.statusCode >= 300) {
    if (error != nullptr) {
      ostringstream out;
      out << "DigiKey token request failed with HTTP " << response.statusCode;
      *error = out.str();
    }
    return nullopt;
  }

  string parseError;
  const auto root = parseJson(response.body, &parseError);
  if (!root.has_value()) {
    if (error != nullptr) {
      *error = "Unable to parse DigiKey token response: " + parseError;
    }
    return nullopt;
  }

  const auto token = readStringPath(*root, {"access_token"});
  if (!token.has_value() || token->empty() || token->size() > 2048U) {
    if (error != nullptr) {
      *error = "DigiKey token response did not include an access token";
    }
    return nullopt;
  }

  tokenExpiresAt_ = time(nullptr) + 540;
  return token;
}

bool DigiKeyApiClient::ensureAccessToken(string* error) {
  if (!accessToken_.empty() && time(nullptr) < tokenExpiresAt_) {
    return true;
  }

  const auto token = requestToken(error);
  if (!token.has_value()) {
    return false;
  }

  accessToken_ = *token;
  return true;
}

optional<string> DigiKeyApiClient::requestProductDetails(const string& productNumber,
                                                                    string* error,
                                                                    const string& manufacturerId) {
  if (trimCopy(productNumber).empty() || productNumber.size() > kMaximumDigiKeyFieldBytes ||
      manufacturerId.size() > kMaximumDigiKeyFieldBytes) {
    if (error != nullptr) *error = "DigiKey product identifier is empty or too large";
    return nullopt;
  }
  if (!ensureAccessToken(error)) {
    return nullopt;
  }

  ostringstream url;
  url << "https://api.digikey.com/products/v4/search/" << encodePathSegment(productNumber) << "/productdetails";
  if (!trimCopy(manufacturerId).empty()) {
    url << "?manufacturerId=" << encodeComponent(manufacturerId, false);
  }

  ostringstream headers;
  if (!appendAuthorizationHeader(headers, accessToken_, error) ||
      !appendHeader(headers, "X-DIGIKEY-Client-Id", config_.clientId, error) ||
      !appendHeader(headers, "X-DIGIKEY-Locale-Language", config_.language, error) ||
      !appendHeader(headers, "X-DIGIKEY-Locale-Currency", config_.currency, error) ||
      !appendHeader(headers, "X-DIGIKEY-Locale-Site", config_.site, error)) {
    return nullopt;
  }
  if (!config_.accountId.empty() && !appendHeader(headers, "X-DIGIKEY-Account-Id", config_.accountId, error)) {
    return nullopt;
  }

  HttpResponse response;
  if (!requestHttp(L"GET", widen(url.str()), widen(headers.str()), "", response, error)) {
    return nullopt;
  }

  if (response.statusCode < 200 || response.statusCode >= 300) {
    if (error != nullptr) {
      ostringstream out;
      out << "DigiKey details request failed with HTTP " << response.statusCode;
      *error = out.str();
    }
    return nullopt;
  }

  return response.body;
}

optional<string> DigiKeyApiClient::requestKeywordSearch(const string& keywords, string* error) {
  if (trimCopy(keywords).empty() || keywords.size() > kMaximumDigiKeyFieldBytes) {
    if (error != nullptr) *error = "DigiKey search keywords are empty or too large";
    return nullopt;
  }
  if (!ensureAccessToken(error)) {
    return nullopt;
  }

  ostringstream body;
  body << "{\"Keywords\":\"" << escapeJsonString(keywords) << "\",\"Limit\":10,\"Offset\":0}";

  ostringstream headers;
  if (!appendAuthorizationHeader(headers, accessToken_, error) ||
      !appendHeader(headers, "X-DIGIKEY-Client-Id", config_.clientId, error) ||
      !appendHeader(headers, "X-DIGIKEY-Locale-Language", config_.language, error) ||
      !appendHeader(headers, "X-DIGIKEY-Locale-Currency", config_.currency, error) ||
      !appendHeader(headers, "X-DIGIKEY-Locale-Site", config_.site, error)) {
    return nullopt;
  }
  if (!config_.accountId.empty() && !appendHeader(headers, "X-DIGIKEY-Account-Id", config_.accountId, error)) {
    return nullopt;
  }
  headers << "Content-Type: application/json\r\n";

  HttpResponse response;
  if (!requestHttp(L"POST", L"https://api.digikey.com/products/v4/search/keyword", widen(headers.str()), body.str(),
                   response, error)) {
    return nullopt;
  }

  if (response.statusCode < 200 || response.statusCode >= 300) {
    if (error != nullptr) {
      ostringstream out;
      out << "DigiKey keyword search failed with HTTP " << response.statusCode;
      *error = out.str();
    }
    return nullopt;
  }

  return response.body;
}

optional<DigiKeyProductDetails> DigiKeyApiClient::fetchProductDetails(const string& productNumber,
                                                                           string* error) {
  if (trimCopy(productNumber).empty() || productNumber.size() > kMaximumDigiKeyFieldBytes) {
    if (error != nullptr) *error = "DigiKey product identifier is empty or too large";
    return nullopt;
  }
  const auto parseDetails = [this](const string& lookupKey, const string& bodyText, string* parseError) {
    string bodyParseError;
    const auto root = parseJson(bodyText, &bodyParseError);
    if (!root.has_value()) {
      if (parseError != nullptr) {
        *parseError = "Unable to parse DigiKey details response: " + bodyParseError;
      }
      return optional<DigiKeyProductDetails>{};
    }

    const auto* rootObject = asObject(*root);
    if (rootObject == nullptr) {
      if (parseError != nullptr) *parseError = "DigiKey details response root is not an object";
      return optional<DigiKeyProductDetails>{};
    }
    if (const auto* product = findMember(*root, "Product"); product != nullptr && asObject(*product) == nullptr) {
      if (parseError != nullptr) *parseError = "DigiKey details response has an invalid Product object";
      return optional<DigiKeyProductDetails>{};
    }

    auto details = parseProductDetails(lookupKey, *root);
    details.vendorMetadata.locale = config_.language;
    if (details.productDescription.empty() && details.parameters.empty()) {
      if (parseError != nullptr) {
        *parseError = "DigiKey returned an empty details payload";
      }
      return optional<DigiKeyProductDetails>{};
    }

    if ((!details.quantityAvailable.empty() && !isUnsignedDecimal(details.quantityAvailable, 1000000000000ULL)) ||
        (!details.manufacturerLeadWeeks.empty() &&
         !isUnsignedDecimal(details.manufacturerLeadWeeks, 1000000ULL)) ||
        (!details.unitPrice.empty() && !isFiniteDecimal(details.unitPrice, 1000000000000.0))) {
      if (parseError != nullptr) *parseError = "DigiKey details response contains an invalid numeric field";
      return optional<DigiKeyProductDetails>{};
    }

    return optional<DigiKeyProductDetails>{move(details)};
  };

  string directError;
  if (const auto body = requestProductDetails(productNumber, &directError); body.has_value()) {
    string parseError;
    if (const auto details = parseDetails(productNumber, *body, &parseError); details.has_value()) {
      return details;
    }
    directError = move(parseError);
  }

  string keywordError;
  const auto keywordBody = requestKeywordSearch(productNumber, &keywordError);
  if (!keywordBody.has_value()) {
    if (error != nullptr) {
      *error = directError.empty() ? keywordError : directError + " | " + keywordError;
    }
    return nullopt;
  }

  string searchParseError;
  const auto searchRoot = parseJson(*keywordBody, &searchParseError);
  if (!searchRoot.has_value()) {
    if (error != nullptr) {
      *error = "Unable to parse DigiKey keyword response: " + searchParseError;
    }
    return nullopt;
  }

  const auto match = resolveSearchResult(*searchRoot, productNumber);
  if (!match.has_value()) {
    if (error != nullptr) {
      *error = directError.empty() ? "DigiKey keyword search did not return a usable match"
                                   : directError + " | DigiKey keyword search did not return a usable match";
    }
    return nullopt;
  }

  string resolvedError;
  if (const auto resolvedBody = requestProductDetails(match->productNumber, &resolvedError, match->manufacturerId);
      resolvedBody.has_value()) {
    string parseError;
    if (const auto details = parseDetails(match->productNumber, *resolvedBody, &parseError); details.has_value()) {
      return details;
    }
    resolvedError = move(parseError);
  }

  if (error != nullptr) {
    *error = directError.empty() ? resolvedError : directError + " | " + resolvedError;
  }
  return nullopt;
}

}  // namespace inventatory

#else

namespace inventatory {

namespace inventatory {

bool DigiKeyConfig::valid() const {
  return false;
}

bool validateDigiKeyJsonPayload(const string&, string* error) {
  if (error != nullptr) *error = "DigiKey integration is only available on Windows";
  return false;
}

DigiKeyConfig loadDigiKeyConfig() {
  return {};
}

DigiKeyApiClient::DigiKeyApiClient(DigiKeyConfig config) : config_(move(config)) {}

bool DigiKeyApiClient::testConnection(string* error) {
  if (error != nullptr) *error = "DigiKey integration is only available on Windows";
  return false;
}

optional<DigiKeyProductDetails> DigiKeyApiClient::fetchProductDetails(const string&, string*) {
  return nullopt;
}

}  // namespace inventatory

#endif
