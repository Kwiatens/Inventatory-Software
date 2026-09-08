// Inventatory - bounded HTTP parsing and replay-state persistence.

#include "platform/scanner/HttpServerProtocolInternal.h"

#include "core/inventory/Inventory.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace inventatory::http_server_detail {

using namespace std;

string jsonEscape(const string& value) {
  ostringstream out;
  for (char ch : value) {
    switch (ch) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        out << ch;
        break;
    }
  }
  return out.str();
}

string trimHttp(const string& value) {
  size_t begin = 0;
  while (begin < value.size() && isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }

  size_t end = value.size();
  while (end > begin && isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }

  return value.substr(begin, end - begin);
}

bool isHeaderNameCharacter(unsigned char ch) {
  return isalnum(ch) != 0 || ch == '!' || ch == '#' || ch == '$' || ch == '%' || ch == '&' || ch == '\'' ||
         ch == '*' || ch == '+' || ch == '-' || ch == '.' || ch == '^' || ch == '_' || ch == '`' || ch == '|' ||
         ch == '~';
}

bool parseHttpHeaders(const string& headers, string& method, string& target, string& version,
                      HttpHeaderMap& values) {
  values.clear();
  for (size_t index = 0; index < headers.size(); ++index) {
    if (headers[index] == '\n' && (index == 0 || headers[index - 1] != '\r')) return false;
  }
  istringstream input(headers);
  string line;
  if (!getline(input, line)) return false;
  if (!line.empty() && line.back() == '\r') line.pop_back();
  istringstream requestLine(line);
  string extra;
  if (!(requestLine >> method >> target >> version) || (requestLine >> extra) || version != "HTTP/1.1") return false;
  if (method.empty() || target.empty() || method.size() > 16 || target.size() > 256) return false;
  for (const unsigned char ch : method) {
    if (!isHeaderNameCharacter(ch)) return false;
  }
  for (const unsigned char ch : target) {
    if (ch <= 0x20U || ch == 0x7fU || ch == '\r' || ch == '\n') return false;
  }

  while (getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto separator = line.find(':');
    if (separator == string::npos || separator == 0 || separator > 128) return false;
    const auto name = toLower(line.substr(0, separator));
    for (const unsigned char ch : name) {
      if (!isHeaderNameCharacter(ch)) return false;
    }
    const auto value = trimHttp(line.substr(separator + 1));
    for (const unsigned char ch : value) {
      if ((ch < 0x20U && ch != '\t') || ch == 0x7fU) return false;
    }
    if (!values.emplace(name, value).second) return false;
  }
  return !input.bad();
}

bool parseContentLength(const HttpHeaderMap& headers, size_t& length) {
  const auto found = headers.find("content-length");
  if (found == headers.end()) {
    length = 0;
    return true;
  }
  if (found->second.empty()) return false;
  length = 0;
  for (const unsigned char ch : found->second) {
    if (!isdigit(ch) || length > (numeric_limits<size_t>::max() - (ch - '0')) / 10U) return false;
    length = length * 10U + (ch - '0');
  }
  return true;
}

optional<uint64_t> headerCounter(const HttpHeaderMap& headers) {
  const auto found = headers.find("x-inventatory-counter");
  if (found == headers.end() || found->second.empty()) return nullopt;
  const auto& value = found->second;
  if (value.empty()) return nullopt;
  uint64_t counter = 0;
  for (const unsigned char ch : value) {
    if (!isdigit(ch) || counter > (numeric_limits<uint64_t>::max() - (ch - '0')) / 10U) return nullopt;
    counter = counter * 10U + (ch - '0');
  }
  return counter == 0 ? nullopt : optional<uint64_t>(counter);
}

bool tokensMatch(const string& expected, const string& supplied) {
  if (expected.empty() || expected.size() != supplied.size()) {
    return false;
  }

  unsigned char difference = 0;
  for (size_t index = 0; index < expected.size(); ++index) {
    difference |= static_cast<unsigned char>(expected[index]) ^ static_cast<unsigned char>(supplied[index]);
  }
  return difference == 0;
}

string httpStatusText(int status) {
  switch (status) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 408: return "408 Request Timeout";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 426: return "426 Upgrade Required";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
  }
}

bool loadReplayState(const filesystem::path& path, const string& fingerprint, uint64_t& counter) {
  counter = 0;
  if (path.empty() || fingerprint.empty()) return true;

  error_code existenceError;
  const bool exists = filesystem::exists(path, existenceError);
  if (existenceError) return false;
  if (!exists) return true;
  error_code sizeError;
  const auto fileSize = filesystem::file_size(path, sizeError);
  if (sizeError || fileSize > 256U) return false;

  ifstream input(path);
  if (!input) return false;

  string storedFingerprint;
  bool hasFingerprint = false;
  bool hasCounter = false;
  string line;
  while (getline(input, line)) {
    const auto separator = line.find('=');
    if (separator == string::npos) return false;

    const auto key = line.substr(0, separator);
    const auto value = line.substr(separator + 1);
    if (key == "fingerprint") {
      if (hasFingerprint || value.size() != 64 ||
          any_of(value.begin(), value.end(), [](unsigned char ch) { return !isxdigit(ch); })) return false;
      storedFingerprint = value;
      hasFingerprint = true;
    } else if (key == "counter") {
      if (hasCounter || value.empty() ||
          any_of(value.begin(), value.end(), [](unsigned char ch) { return !isdigit(ch); })) {
        return false;
      }
      try {
        size_t parsed = 0;
        counter = stoull(value, &parsed);
        if (parsed != value.size()) return false;
      } catch (...) {
        return false;
      }
      hasCounter = true;
    } else {
      return false;
    }
  }
  return !input.bad() && hasFingerprint && hasCounter && storedFingerprint == fingerprint;
}

bool saveReplayState(const filesystem::path& path, const string& fingerprint, uint64_t counter) {
  if (path.empty() || fingerprint.size() != 64 ||
      any_of(fingerprint.begin(), fingerprint.end(), [](unsigned char ch) { return !isxdigit(ch); })) return false;
  error_code error;
  filesystem::create_directories(path.parent_path(), error);
  if (error) return false;

  const string contents = "fingerprint=" + fingerprint + '\n' + "counter=" + to_string(counter) + '\n';
  static atomic<uint64_t> temporarySequence{0};
  auto temporary = path;
  temporary += L".tmp.";
  temporary += to_wstring(GetCurrentProcessId());
  temporary += L".";
  temporary += to_wstring(GetCurrentThreadId());
  temporary += L".";
  temporary += to_wstring(temporarySequence.fetch_add(1, memory_order_relaxed));
#ifdef _WIN32
  // CREATE_NEW prevents two writers from sharing a predictable temporary file.
  // WRITE_THROUGH plus FlushFileBuffers makes a successful replacement durable
  // enough for the existing replay invariant; the old state is untouched until
  // the complete temporary file has been closed.
  HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;

  bool success = contents.size() <= numeric_limits<DWORD>::max();
  DWORD written = 0;
  if (success) {
    success = WriteFile(handle, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) != 0 &&
              written == contents.size();
  }
  if (success) success = FlushFileBuffers(handle) != 0;
  if (CloseHandle(handle) == 0) success = false;
  if (!success) {
    filesystem::remove(temporary, error);
    return false;
  }
  if (MoveFileExW(temporary.c_str(), path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    filesystem::remove(temporary, error);
    return false;
  }
  return true;
#else
  ofstream output(temporary, ios::binary);
  if (!output) return false;
  output.write(contents.data(), static_cast<streamsize>(contents.size()));
  output.flush();
  const bool success = output.good();
  output.close();
  if (!success || !output) {
    filesystem::remove(temporary, error);
    return false;
  }
  filesystem::rename(temporary, path, error);
  const bool renamed = !error;
  if (!renamed) {
    error_code cleanupError;
    filesystem::remove(temporary, cleanupError);
  }
  return renamed;
#endif
}

}  // namespace inventatory::http_server_detail
