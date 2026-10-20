// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// The token request sends HTTP Basic credentials. Product symbols and part numbers
// go in the query string.

#pragma once

#include <string>

namespace inventatory {
namespace tme_encoding {

// Standard Base64 with padding.
std::string base64Encode(const std::string& data);
// Percent-encodes every byte outside the RFC 3986 unreserved set, the same as Python's quote(value, "").
std::string percentEncode(const std::string& value);

}  // namespace tme_encoding
}  // namespace inventatory
