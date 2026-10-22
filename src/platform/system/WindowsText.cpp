// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "platform/system/WindowsText.h"

#include <windows.h>

#include <limits>

namespace inventatory {

std::wstring utf8ToWide(const std::string& value) {
  if (value.empty() || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
  const int size = static_cast<int>(value.size());
  const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), size, nullptr, 0);
  if (count <= 0) return {};
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, value.data(), size, result.data(), count) <= 0) return {};
  return result;
}

std::string wideToUtf8(const wchar_t* value, std::size_t length) {
  if (value == nullptr || length == 0 || length > static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
  const int size = static_cast<int>(length);
  const int count = WideCharToMultiByte(CP_UTF8, 0, value, size, nullptr, 0, nullptr, nullptr);
  if (count <= 0) return {};
  std::string result(static_cast<std::size_t>(count), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, value, size, result.data(), count, nullptr, nullptr) <= 0) return {};
  return result;
}

}  // namespace inventatory
