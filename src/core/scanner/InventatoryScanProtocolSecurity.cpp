// Inventatory - Inventatory Scan R1 transport authentication helpers.

#include "core/scanner/InventatoryScanProtocol.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "Bcrypt.lib")
#else
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#endif

namespace inventatory {

using namespace std;

namespace {

int hexDigit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

string hexBytes(const unsigned char* bytes, size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  string out;
  out.reserve(size * 2U);
  for (size_t index = 0; index < size; ++index) {
    out.push_back(kHex[(bytes[index] >> 4U) & 0x0fU]);
    out.push_back(kHex[bytes[index] & 0x0fU]);
  }
  return out;
}

bool decodeToken(const string& token, array<unsigned char, 32>& bytes) {
  if (token.size() != bytes.size() * 2U) return false;
  for (size_t index = 0; index < bytes.size(); ++index) {
    const auto high = hexDigit(token[index * 2U]);
    const auto low = hexDigit(token[index * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    bytes[index] = static_cast<unsigned char>((high << 4U) | low);
  }
  return true;
}

bool hmacSha256(const unsigned char* key, size_t keySize, const string& input, array<unsigned char, 32>& output) {
#ifdef _WIN32
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD objectSize = 0;
  DWORD hashSize = 0;
  ULONG ignored = 0;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0 ||
      BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                        &ignored, 0) != 0 ||
      BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize),
                        &ignored, 0) != 0 || hashSize != output.size()) {
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    return false;
  }
  vector<unsigned char> object(objectSize);
  if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, const_cast<PUCHAR>(key),
                       static_cast<ULONG>(keySize), 0) != 0 ||
      BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                     static_cast<ULONG>(input.size()), 0) != 0 ||
      BCryptFinishHash(hash, output.data(), static_cast<ULONG>(output.size()), 0) != 0) {
    if (hash != nullptr) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return false;
  }
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return true;
#else
  EVP_MAC* algorithm = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  if (algorithm == nullptr) return false;
  EVP_MAC_CTX* context = EVP_MAC_CTX_new(algorithm);
  EVP_MAC_free(algorithm);
  if (context == nullptr) return false;
  char digestName[] = "SHA256";
  OSSL_PARAM parameters[] = {OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digestName, 0),
                             OSSL_PARAM_construct_end()};
  size_t outputLength = 0;
  const bool success = EVP_MAC_init(context, key, keySize, parameters) == 1 &&
                       EVP_MAC_update(context, reinterpret_cast<const unsigned char*>(input.data()), input.size()) == 1 &&
                       EVP_MAC_final(context, output.data(), &outputLength, output.size()) == 1 &&
                       outputLength == output.size();
  EVP_MAC_CTX_free(context);
  return success;
#endif
}

string transportMac(const string& token, const char* direction, const string& input) {
  array<unsigned char, 32> root{};
  array<unsigned char, 32> key{};
  array<unsigned char, 32> mac{};
  if (!decodeToken(token, root)) return {};
  if (!hmacSha256(root.data(), root.size(), string("Inventatory Scan R1 transport v1 ") + direction, key) ||
      !hmacSha256(key.data(), key.size(), input, mac)) {
    return {};
  }
  return hexBytes(mac.data(), mac.size());
}

string hexToken(const array<unsigned char, 32>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  string result;
  result.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    result.push_back(kHex[(byte >> 4U) & 0x0fU]);
    result.push_back(kHex[byte & 0x0fU]);
  }
  return result;
}

}  // namespace

string generateInventatoryScanToken() {
  array<unsigned char, 32> bytes{};
#ifdef _WIN32
  if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
    return hexToken(bytes);
  }
#else
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) == 1) return hexToken(bytes);
#endif
  return {};
}

filesystem::path inventatoryScanReplayStatePath(const filesystem::path& workspaceDirectory) {
  if (workspaceDirectory.empty()) return {};
  error_code error;
  const auto absoluteDirectory = filesystem::absolute(workspaceDirectory, error);
  if (error || absoluteDirectory.empty()) return {};
  return absoluteDirectory.lexically_normal() / "inventatory-scan-replay.state";
}

string deviceRequestMac(const string& token, const string& method, const string& path, const string& deviceId,
                        uint64_t counter, const string& body) {
  return transportMac(token, "client-to-server",
                      "Inventatory Scan R1/v1\nrequest\n" + method + '\n' + path + '\n' + deviceId + '\n' +
                          to_string(counter) + '\n' + body);
}

string deviceResponseMac(const string& token, uint64_t counter, int status, const string& body) {
  return transportMac(token, "server-to-client",
                      "Inventatory Scan R1/v1\nresponse\n" + to_string(counter) + '\n' + to_string(status) + '\n' +
                          body);
}

string deviceTransportStateFingerprint(const string& token) {
  return transportMac(token, "replay-state", "Inventatory Scan R1/v1 replay state");
}

}  // namespace inventatory
