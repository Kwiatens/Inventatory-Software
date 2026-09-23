// Inventatory - Linux Secret Service credential storage.

#include "platform/security/CredentialStore.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>

#include <openssl/evp.h>
#include <secret/secret.h>

namespace inventatory {
namespace filesystem = std::filesystem;
namespace {

const SecretSchema kCredentialSchema = {
    "org.kwiatens.Inventatory", SECRET_SCHEMA_NONE, {{"key", SECRET_SCHEMA_ATTRIBUTE_STRING}}};

std::string credentialAttribute(const std::string& key) { return "Inventatory/" + key; }

std::optional<std::string> workspaceDigest(const filesystem::path& workspaceDirectory) {
  if (workspaceDirectory.empty()) return std::nullopt;
  std::error_code error;
  const auto normalized = filesystem::weakly_canonical(workspaceDirectory, error).lexically_normal();
  if (error || normalized.empty()) return std::nullopt;
  const auto encoded = normalized.u8string();
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digestSize = 0;
  if (EVP_Digest(encoded.data(), encoded.size(), digest.data(), &digestSize, EVP_sha256(), nullptr) != 1 ||
      digestSize != 32U) return std::nullopt;
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(digestSize * 2U);
  for (unsigned int index = 0; index < digestSize; ++index) {
    result.push_back(kHex[(digest[index] >> 4U) & 0x0fU]);
    result.push_back(kHex[digest[index] & 0x0fU]);
  }
  return result;
}

}  // namespace

std::optional<std::string> CredentialStore::read(const std::string& key) {
  if (key.empty()) return std::nullopt;
  GError* error = nullptr;
  gchar* secret = secret_password_lookup_sync(&kCredentialSchema, nullptr, &error,
                                              "key", credentialAttribute(key).c_str(), nullptr);
  if (error != nullptr) g_error_free(error);
  if (secret == nullptr) return std::nullopt;
  std::string value(secret);
  secret_password_free(secret);
  return value;
}

bool CredentialStore::write(const std::string& key, const std::string& secret) {
  if (key.empty()) return false;
  if (secret.empty()) return erase(key);
  GError* error = nullptr;
  const auto attribute = credentialAttribute(key);
  const gboolean stored = secret_password_store_sync(&kCredentialSchema, SECRET_COLLECTION_DEFAULT,
                                                      "Inventatory credential", secret.c_str(), nullptr,
                                                      &error, "key", attribute.c_str(), nullptr);
  if (error != nullptr) g_error_free(error);
  return stored != FALSE;
}

bool CredentialStore::erase(const std::string& key) {
  if (key.empty()) return false;
  GError* error = nullptr;
  const auto attribute = credentialAttribute(key);
  const gboolean cleared = secret_password_clear_sync(&kCredentialSchema, nullptr, &error,
                                                       "key", attribute.c_str(), nullptr);
  if (error != nullptr) g_error_free(error);
  // libsecret treats removing an absent item as a successful no-op.
  return cleared != FALSE;
}

std::string CredentialStore::workspaceScopedKey(const filesystem::path& workspaceDirectory,
                                                 const std::string& key) {
  if (key.empty()) return {};
  const auto digest = workspaceDigest(workspaceDirectory);
  return digest.has_value() ? key + "@" + *digest : std::string();
}

std::optional<std::string> CredentialStore::readForWorkspace(const filesystem::path& workspaceDirectory,
                                                              const std::string& key) {
  const auto scopedKey = workspaceScopedKey(workspaceDirectory, key);
  return scopedKey.empty() ? std::nullopt : read(scopedKey);
}

bool CredentialStore::writeForWorkspace(const filesystem::path& workspaceDirectory, const std::string& key,
                                         const std::string& secret) {
  const auto scopedKey = workspaceScopedKey(workspaceDirectory, key);
  return !scopedKey.empty() && write(scopedKey, secret);
}

bool CredentialStore::eraseForWorkspace(const filesystem::path& workspaceDirectory, const std::string& key) {
  const auto scopedKey = workspaceScopedKey(workspaceDirectory, key);
  return !scopedKey.empty() && erase(scopedKey);
}

}  // namespace inventatory
