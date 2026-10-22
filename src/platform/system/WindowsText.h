// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// UTF-8 and UTF-16 conversion for Windows API calls. This header does not include <windows.h>.

#pragma once

#ifdef _WIN32

#include <cstddef>
#include <string>

namespace inventatory {

// Empty for empty input, or for text the Windows API cannot convert.
std::wstring utf8ToWide(const std::string& value);
std::string wideToUtf8(const wchar_t* value, std::size_t length);
inline std::string wideToUtf8(const std::wstring& value) { return wideToUtf8(value.data(), value.size()); }

}  // namespace inventatory

#endif
