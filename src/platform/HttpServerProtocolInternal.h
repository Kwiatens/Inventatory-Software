// Inventatory - internal HTTP parsing and replay-state helpers.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace inventatory::http_server_detail {

using HttpHeaderMap = std::unordered_map<std::string, std::string>;

std::string jsonEscape(const std::string& value);
std::string trimHttp(const std::string& value);
bool parseHttpHeaders(const std::string& headers, std::string& method, std::string& target,
                      std::string& version, HttpHeaderMap& values);
bool parseContentLength(const HttpHeaderMap& headers, std::size_t& length);
std::optional<std::uint64_t> headerCounter(const HttpHeaderMap& headers);
bool tokensMatch(const std::string& expected, const std::string& supplied);
std::string httpStatusText(int status);
bool loadReplayState(const std::filesystem::path& path, const std::string& fingerprint,
                     std::uint64_t& counter);
bool saveReplayState(const std::filesystem::path& path, const std::string& fingerprint,
                     std::uint64_t counter);

}  // namespace inventatory::http_server_detail
