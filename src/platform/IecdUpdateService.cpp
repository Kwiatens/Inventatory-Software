// Signed IECD snapshot download, validation, and atomic installation.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "platform/IecdUpdateService.h"

#include "core/InventorySqlite.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <monocypher-ed25519.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace inventatory {

using namespace std;

namespace {

constexpr wchar_t kManifestUrl[] =
    L"https://github.com/Kwiatens/Inventatory-Electronics-Components-Database/releases/latest/download/iecd-manifest.json";
constexpr wchar_t kDatabaseUrl[] =
    L"https://github.com/Kwiatens/Inventatory-Electronics-Components-Database/releases/latest/download/iecd.sqlite3";

string jsonString(const string& json, const string& key) {
  const auto marker = '"' + key + '"';
  const auto keyAt = json.find(marker);
  const auto colon = keyAt == string::npos ? string::npos : json.find(':', keyAt + marker.size());
  const auto quote = colon == string::npos ? string::npos : json.find('"', colon + 1);
  if (quote == string::npos) return {};
  string value;
  bool escaped = false;
  for (size_t index = quote + 1; index < json.size(); ++index) {
    const char ch = json[index];
    if (escaped) {
      switch (ch) {
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: value.push_back(ch); break;
      }
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (ch == '"') {
      return value;
    } else {
      value.push_back(ch);
    }
  }
  return {};
}

long long jsonInteger(const string& json, const string& key, long long fallback = -1) {
  const auto marker = '"' + key + '"';
  const auto keyAt = json.find(marker);
  const auto colon = keyAt == string::npos ? string::npos : json.find(':', keyAt + marker.size());
  if (colon == string::npos) return fallback;
  size_t begin = colon + 1;
  while (begin < json.size() && isspace(static_cast<unsigned char>(json[begin]))) ++begin;
  size_t end = begin;
  while (end < json.size() && isdigit(static_cast<unsigned char>(json[end]))) ++end;
  if (end == begin) return fallback;
  try { return stoll(json.substr(begin, end - begin)); } catch (...) { return fallback; }
}

string jsonEscape(const string& value) {
  string out;
  for (unsigned char ch : value) {
    if (ch == '"' || ch == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(ch)); }
    else if (ch == '\n') out += "\\n";
    else if (ch == '\r') out += "\\r";
    else if (ch == '\t') out += "\\t";
    else out.push_back(static_cast<char>(ch));
  }
  return out;
}

string canonicalManifest(const string& manifest) {
  const auto databaseFile = jsonString(manifest, "database_file");
  const auto databaseVersion = jsonString(manifest, "database_version");
  const auto recordCount = jsonInteger(manifest, "record_count");
  const auto schemaVersion = jsonInteger(manifest, "schema_version");
  const auto sha256 = jsonString(manifest, "sha256");
  const auto algorithm = jsonString(manifest, "signature_algorithm");
  const auto sourceCommit = jsonString(manifest, "source_commit");
  if (databaseFile.empty() || databaseVersion.empty() || recordCount < 0 || schemaVersion < 0 || sha256.empty() ||
      algorithm.empty() || sourceCommit.empty()) return {};
  return "{\"database_file\":\"" + jsonEscape(databaseFile) + "\",\"database_version\":\"" +
         jsonEscape(databaseVersion) + "\",\"record_count\":" + to_string(recordCount) +
         ",\"schema_version\":" + to_string(schemaVersion) + ",\"sha256\":\"" + jsonEscape(sha256) +
         "\",\"signature\":\"\",\"signature_algorithm\":\"" + jsonEscape(algorithm) +
         "\",\"source_commit\":\"" + jsonEscape(sourceCommit) + "\"}";
}

vector<unsigned char> decodeBase64(const string& value) {
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  vector<unsigned char> result;
  unsigned int buffer = 0;
  int bits = 0;
  for (unsigned char ch : value) {
    if (ch == '=') break;
    const auto* found = find(begin(alphabet), end(alphabet) - 1, static_cast<char>(ch));
    if (found == end(alphabet) - 1) return {};
    buffer = (buffer << 6U) | static_cast<unsigned int>(found - begin(alphabet));
    bits += 6;
    if (bits >= 8) { bits -= 8; result.push_back(static_cast<unsigned char>((buffer >> bits) & 0xffU)); }
  }
  return result;
}

string sha256File(const filesystem::path& path) {
  ifstream input(path, ios::binary);
  if (!input) return {};
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD objectSize = 0;
  DWORD returned = 0;
  array<unsigned char, 32> digest{};
  vector<unsigned char> object;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
      BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                        &returned, 0) != 0) {
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return {};
  }
  object.resize(objectSize);
  if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) != 0) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return {};
  }
  array<char, 64 * 1024> block{};
  while (input) {
    input.read(block.data(), block.size());
    const auto count = input.gcount();
    if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(block.data()), static_cast<ULONG>(count), 0) != 0) {
      BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); return {};
    }
  }
  const bool ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (!ok) return {};
  static constexpr char hex[] = "0123456789abcdef";
  string result;
  for (const auto byte : digest) { result.push_back(hex[byte >> 4U]); result.push_back(hex[byte & 0x0fU]); }
  return result;
}

bool compatibleDatabase(const filesystem::path& path, const string& expectedVersion, long long expectedRecordCount) {
  SqliteConnection connection;
  if (!openReadOnlyDatabase(path, connection)) return false;
  SqliteStatement statement;
  if (sqliteApi().prepare_v2(connection.db,
      "SELECT key, value FROM metadata WHERE key IN ('schema_version','database_version')", -1,
      &statement.stmt, nullptr) != SQLITE_OK) return false;
  string schema;
  string version;
  while (sqliteApi().step(statement.stmt) == SQLITE_ROW) {
    const auto key = sqliteText(statement.stmt, 0);
    if (key == "schema_version") schema = sqliteText(statement.stmt, 1);
    else if (key == "database_version") version = sqliteText(statement.stmt, 1);
  }
  if (schema != "1" || version != expectedVersion) return false;
  SqliteStatement countStatement;
  if (sqliteApi().prepare_v2(connection.db, "SELECT COUNT(*) FROM components", -1,
                             &countStatement.stmt, nullptr) != SQLITE_OK ||
      sqliteApi().step(countStatement.stmt) != SQLITE_ROW) return false;
  return sqliteApi().column_int64(countStatement.stmt, 0) == expectedRecordCount;
}

bool downloadFile(const wchar_t* url, const filesystem::path& destination, size_t maximumBytes) {
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256]{};
  wchar_t path[2048]{};
  parts.lpszHostName = host; parts.dwHostNameLength = static_cast<DWORD>(size(host));
  parts.lpszUrlPath = path; parts.dwUrlPathLength = static_cast<DWORD>(size(path));
  if (!WinHttpCrackUrl(url, 0, 0, &parts)) return false;
  const auto session = WinHttpOpen(L"Inventatory-IECD/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  const auto connection = WinHttpConnect(session, host, parts.nPort, 0);
  const auto request = connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
  DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
  if (request) WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
  bool ok = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
      WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
  DWORD status = 0; DWORD statusSize = sizeof(status);
  if (ok) ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                   nullptr, &status, &statusSize, nullptr) && status == 200;
  ofstream output;
  if (ok) output.open(destination, ios::binary | ios::trunc);
  size_t total = 0;
  array<char, 32 * 1024> buffer{};
  while (ok) {
    DWORD read = 0;
    if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) { ok = false; break; }
    if (read == 0) break;
    total += read;
    if (total > maximumBytes) { ok = false; break; }
    output.write(buffer.data(), read);
    ok = output.good();
  }
  output.close();
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  if (!ok) { error_code ignored; filesystem::remove(destination, ignored); }
  return ok;
}

}  // namespace

const array<unsigned char, 32>& iecdReleasePublicKey() {
  static const array<unsigned char, 32> key = {
      0xc2,0xe6,0xd9,0x5c,0x02,0xcd,0x89,0x92,0x9d,0xd2,0x4c,0x9a,0xdb,0x25,0xab,0xba,
      0xc5,0x80,0x00,0xc3,0x28,0x5e,0xd7,0xd3,0x26,0x6e,0xc2,0xf3,0xe3,0x77,0xdf,0xa1};
  return key;
}

IecdInstallResult installIecdSnapshot(const string& manifestJson, const filesystem::path& candidateDatabase,
                                      const filesystem::path& installedDatabase) {
  IecdInstallResult result{true, false, jsonString(manifestJson, "database_version"), {}};
  if (jsonInteger(manifestJson, "schema_version") != 1 || jsonString(manifestJson, "database_file") != "iecd.sqlite3" ||
      jsonString(manifestJson, "signature_algorithm") != "Ed25519") {
    result.error = "Incompatible IECD manifest"; return result;
  }
  const auto canonical = canonicalManifest(manifestJson);
  const auto signature = decodeBase64(jsonString(manifestJson, "signature"));
  if (canonical.empty() || signature.size() != 64 || crypto_ed25519_check(signature.data(), iecdReleasePublicKey().data(),
      reinterpret_cast<const unsigned char*>(canonical.data()), canonical.size()) != 0) {
    result.error = "IECD manifest signature is invalid"; return result;
  }
  auto expectedDigest = jsonString(manifestJson, "sha256");
  transform(expectedDigest.begin(), expectedDigest.end(), expectedDigest.begin(), [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
  if (sha256File(candidateDatabase) != expectedDigest) { result.error = "IECD database digest does not match"; return result; }
  if (!compatibleDatabase(candidateDatabase, result.databaseVersion, jsonInteger(manifestJson, "record_count"))) {
    result.error = "IECD database schema or record count is incompatible"; return result;
  }

  error_code error;
  filesystem::create_directories(installedDatabase.parent_path(), error);
  const auto incoming = filesystem::path(installedDatabase.string() + ".incoming");
  const auto previous = filesystem::path(installedDatabase.string() + ".previous");
  filesystem::remove(incoming, error); error.clear();
  filesystem::copy_file(candidateDatabase, incoming, filesystem::copy_options::overwrite_existing, error);
  if (error) { result.error = "Unable to stage IECD database"; return result; }
  filesystem::remove(previous, error); error.clear();
  if (filesystem::exists(installedDatabase)) {
    filesystem::rename(installedDatabase, previous, error);
    if (error) { filesystem::remove(incoming, error); result.error = "Unable to retain previous IECD database"; return result; }
  }
  filesystem::rename(incoming, installedDatabase, error);
  if (error) {
    error_code rollbackError;
    if (filesystem::exists(previous)) filesystem::rename(previous, installedDatabase, rollbackError);
    filesystem::remove(incoming, rollbackError);
    result.error = "Unable to activate IECD database"; return result;
  }
  result.installed = true;
  return result;
}

IecdInstallResult downloadAndInstallLatestIecd(const filesystem::path& installedDatabase) {
  const auto manifestPath = filesystem::path(installedDatabase.string() + ".manifest.download");
  const auto databasePath = filesystem::path(installedDatabase.string() + ".download");
  if (!downloadFile(kManifestUrl, manifestPath, 256 * 1024) || !downloadFile(kDatabaseUrl, databasePath, 128 * 1024 * 1024)) {
    return {true, false, {}, "IECD update download failed"};
  }
  ifstream input(manifestPath, ios::binary);
  const string manifest((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
  auto result = installIecdSnapshot(manifest, databasePath, installedDatabase);
  error_code ignored;
  filesystem::remove(manifestPath, ignored);
  filesystem::remove(databasePath, ignored);
  return result;
}

}  // namespace inventatory
