// Inventatory - private Scan R1 protocol implementation contracts.

#pragma once

#include "core/InventatoryScanProtocol.h"

#include <optional>
#include <string>
#include <vector>

namespace inventatory::scan_protocol_detail {

std::string jsonEscape(const std::string& value);
bool jsonObjectIsComplete(const std::string& body);
std::optional<std::string> jsonString(const std::string& body, const std::string& key);
std::optional<int> jsonInt(const std::string& body, const std::string& key);
std::optional<bool> jsonBool(const std::string& body, const std::string& key);
std::optional<std::string> jsonObjectBody(const std::string& body, const std::string& key);
std::optional<std::vector<std::string>> jsonObjectArray(const std::string& body, const std::string& key);
std::optional<std::vector<std::string>> jsonStringArray(const std::string& body, const std::string& key);

}  // namespace inventatory::scan_protocol_detail
