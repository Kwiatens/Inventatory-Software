// Inventatory - Hardware Inventory Management System
// Windows Credential Manager implementation for user-scoped secrets.

#include "platform/CredentialStore.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <vector>

#pragma comment(lib, "Bcrypt.lib")

namespace inventatory {

using namespace std;
namespace filesystem = std::filesystem;

namespace {

wstring widen(const string& value) {
  if (value.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  wstring result(static_cast<size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
  return result;
}

string narrow(const wchar_t* value, size_t length) {
  if (value == nullptr || length == 0) return {};
  const int count = WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
  string result(static_cast<size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length), result.data(), count, nullptr, nullptr);
  return result;
}

wstring targetName(const string& key) {
  return L"Inventatory/" + widen(key);
}

string hexBytes(const unsigned char* bytes, size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  string result;
  result.reserve(size * 2U);
  for (size_t index = 0; index < size; ++index) {
    result.push_back(kHex[(bytes[index] >> 4U) & 0x0fU]);
    result.push_back(kHex[bytes[index] & 0x0fU]);
  }
  return result;
}

optional<string> workspaceDigest(const filesystem::path& workspaceDirectory) {
  if (workspaceDirectory.empty()) return nullopt;
  error_code canonicalError;
  auto normalizedPath = filesystem::weakly_canonical(workspaceDirectory, canonicalError).lexically_normal().wstring();
  if (canonicalError || normalizedPath.empty() ||
      normalizedPath.size() > (numeric_limits<ULONG>::max)() / sizeof(wchar_t)) {
    return nullopt;
  }

  // Windows paths are case-insensitive.  Lowercasing the UTF-16 path keeps
  // equivalent spellings in the same Credential Manager scope while leaving
  // the original path untouched on disk.
  transform(normalizedPath.begin(), normalizedPath.end(), normalizedPath.begin(), [](wchar_t ch) {
    return ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
  });

  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD objectSize = 0;
  DWORD hashSize = 0;
  ULONG ignored = 0;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
      BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                        &ignored, 0) != 0 ||
      BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize),
                        &ignored, 0) != 0 ||
      hashSize != 32U) {
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    return nullopt;
  }

  vector<unsigned char> object(objectSize);
  array<unsigned char, 32> digest{};
  const auto byteLength = static_cast<ULONG>(normalizedPath.size() * sizeof(wchar_t));
  if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) != 0 ||
      BCryptHashData(hash, reinterpret_cast<PUCHAR>(normalizedPath.data()), byteLength, 0) != 0 ||
      BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) != 0) {
    if (hash != nullptr) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return nullopt;
  }
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return hexBytes(digest.data(), digest.size());
}

}  // namespace

optional<string> CredentialStore::read(const string& key) {
  PCREDENTIALW credential = nullptr;
  const auto target = targetName(key);
  if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) return nullopt;
  const auto* text = reinterpret_cast<const wchar_t*>(credential->CredentialBlob);
  const auto length = credential->CredentialBlobSize / sizeof(wchar_t);
  auto result = narrow(text, length);
  CredFree(credential);
  return result;
}

bool CredentialStore::write(const string& key, const string& secret) {
  if (secret.empty()) return erase(key);
  auto target = targetName(key);
  auto wideSecret = widen(secret);
  CREDENTIALW credential{};
  credential.Type = CRED_TYPE_GENERIC;
  credential.TargetName = target.data();
  credential.CredentialBlobSize = static_cast<DWORD>(wideSecret.size() * sizeof(wchar_t));
  credential.CredentialBlob = reinterpret_cast<LPBYTE>(wideSecret.data());
  credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
  credential.UserName = const_cast<wchar_t*>(L"Inventatory user");
  return CredWriteW(&credential, 0) != FALSE;
}

bool CredentialStore::erase(const string& key) {
  const auto target = targetName(key);
  if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) return true;
  return GetLastError() == ERROR_NOT_FOUND;
}

string CredentialStore::workspaceScopedKey(const filesystem::path& workspaceDirectory, const string& key) {
  if (key.empty()) return {};
  const auto digest = workspaceDigest(workspaceDirectory);
  if (!digest.has_value()) return {};
  return key + "@" + *digest;
}

optional<string> CredentialStore::readForWorkspace(const filesystem::path& workspaceDirectory, const string& key) {
  const auto scopedKey = workspaceScopedKey(workspaceDirectory, key);
  return scopedKey.empty() ? nullopt : read(scopedKey);
}

bool CredentialStore::writeForWorkspace(const filesystem::path& workspaceDirectory, const string& key,
                                        const string& secret) {
  const auto scopedKey = workspaceScopedKey(workspaceDirectory, key);
  return !scopedKey.empty() && write(scopedKey, secret);
}

bool CredentialStore::eraseForWorkspace(const filesystem::path& workspaceDirectory, const string& key) {
  const auto scopedKey = workspaceScopedKey(workspaceDirectory, key);
  return !scopedKey.empty() && erase(scopedKey);
}

}  // namespace inventatory
