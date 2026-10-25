// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// SHA-256 and ECDSA P-256 verification for Scan R1 firmware packages.

#include "core/scanner/InventatoryScanFirmware.h"

#include <cstring>
#include <string>

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
#include <array>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#endif

namespace inventatory {

using namespace std;

namespace {

constexpr size_t kSha256Size = 32;

}  // namespace

#ifdef _WIN32

string scanFirmwareSha256(const string& data) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
  string digest(kSha256Size, '\0');
  const auto status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                 static_cast<ULONG>(data.size()), reinterpret_cast<PUCHAR>(digest.data()),
                                 static_cast<ULONG>(digest.size()));
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return status == 0 ? digest : string();
}

bool scanFirmwareSignatureValid(const string& publicKey, const string& signedData, const string& signature) {
  if (publicKey.size() != 65U || publicKey[0] != '\x04' || signature.size() != 64U) return false;
  const auto digest = scanFirmwareSha256(signedData);
  if (digest.size() != kSha256Size) return false;

  // BCRYPT_ECCKEY_BLOB: magic and key size, then X and Y. Windows is little-endian.
  string blob(8U + 64U, '\0');
  const ULONG magic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
  const ULONG keySize = 32;
  memcpy(blob.data(), &magic, sizeof(magic));
  memcpy(blob.data() + 4U, &keySize, sizeof(keySize));
  memcpy(blob.data() + 8U, publicKey.data() + 1U, 64U);

  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) return false;
  BCRYPT_KEY_HANDLE key = nullptr;
  bool valid = false;
  if (BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                          reinterpret_cast<PUCHAR>(blob.data()), static_cast<ULONG>(blob.size()), 0) == 0) {
    // BCrypt takes the raw r || s signature, the same layout the firmware signs.
    valid = BCryptVerifySignature(key, nullptr, reinterpret_cast<PUCHAR>(const_cast<char*>(digest.data())),
                                  static_cast<ULONG>(digest.size()),
                                  reinterpret_cast<PUCHAR>(const_cast<char*>(signature.data())),
                                  static_cast<ULONG>(signature.size()), 0) == 0;
  }
  if (key != nullptr) BCryptDestroyKey(key);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return valid;
}

#else

namespace {

// OpenSSL verifies ECDSA signatures in DER form, so the raw r || s is wrapped as ECDSA-Sig-Value.
string derEcdsaSignature(const string& raw) {
  const auto integer = [](const char* bytes) {
    size_t start = 0;
    while (start < 31U && bytes[start] == '\0') ++start;
    string value(bytes + start, 32U - start);
    if ((static_cast<unsigned char>(value[0]) & 0x80U) != 0) value.insert(value.begin(), '\0');
    return string("\x02", 1) + static_cast<char>(value.size()) + value;
  };
  const string sequence = integer(raw.data()) + integer(raw.data() + 32);
  return string("\x30", 1) + static_cast<char>(sequence.size()) + sequence;
}

}  // namespace

string scanFirmwareSha256(const string& data) {
  array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (EVP_Digest(data.data(), data.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 ||
      length != kSha256Size) {
    return {};
  }
  return string(reinterpret_cast<const char*>(digest.data()), length);
}

bool scanFirmwareSignatureValid(const string& publicKey, const string& signedData, const string& signature) {
  if (publicKey.size() != 65U || publicKey[0] != '\x04' || signature.size() != 64U) return false;
  const auto digest = scanFirmwareSha256(signedData);
  if (digest.size() != kSha256Size) return false;

  OSSL_PARAM_BLD* builder = OSSL_PARAM_BLD_new();
  if (builder == nullptr) return false;
  OSSL_PARAM_BLD_push_utf8_string(builder, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
  OSSL_PARAM_BLD_push_octet_string(builder, OSSL_PKEY_PARAM_PUB_KEY, publicKey.data(), publicKey.size());
  OSSL_PARAM* parameters = OSSL_PARAM_BLD_to_param(builder);
  EVP_PKEY_CTX* keyContext = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
  EVP_PKEY* key = nullptr;
  bool valid = false;
  if (parameters != nullptr && keyContext != nullptr && EVP_PKEY_fromdata_init(keyContext) == 1 &&
      EVP_PKEY_fromdata(keyContext, &key, EVP_PKEY_PUBLIC_KEY, parameters) == 1 && key != nullptr) {
    EVP_PKEY_CTX* verifyContext = EVP_PKEY_CTX_new(key, nullptr);
    if (verifyContext != nullptr) {
      const auto der = derEcdsaSignature(signature);
      valid = EVP_PKEY_verify_init(verifyContext) == 1 &&
              EVP_PKEY_verify(verifyContext, reinterpret_cast<const unsigned char*>(der.data()), der.size(),
                              reinterpret_cast<const unsigned char*>(digest.data()), digest.size()) == 1;
      EVP_PKEY_CTX_free(verifyContext);
    }
  }
  EVP_PKEY_free(key);
  EVP_PKEY_CTX_free(keyContext);
  OSSL_PARAM_free(parameters);
  OSSL_PARAM_BLD_free(builder);
  return valid;
}

#endif

}  // namespace inventatory
