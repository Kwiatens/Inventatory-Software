// Inventatory - Hardware Inventory Management System
// Windows Credential Manager implementation for user-scoped secrets.

#include "platform/CredentialStore.h"

#include <windows.h>
#include <wincred.h>

#include <vector>

namespace inventatory {

using namespace std;

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
}

bool CredentialStore::erase(const string& key) {
  const auto target = targetName(key);
  if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) return true;
  return GetLastError() == ERROR_NOT_FOUND;
}

}  // namespace inventatory
