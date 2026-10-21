// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Crash-resistant replacement for small local text files.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace inventatory {

// Writes via a temporary file in the destination directory, then replaces the destination. The
// destination is never truncated first, and failure removes only this call's temp file. Does not
// throw, so callers keep in-memory state and can retry. On Linux the result is durable on return.
// A symlink destination is written through to its target.
bool writeFileAtomically(const std::filesystem::path& destination, std::string_view contents,
                         std::string* error = nullptr);

// Flushes an existing file to stable storage before it is published by a rename.
bool syncFile(const std::filesystem::path& file, std::string* error = nullptr);

// Flushes a directory's entries to stable storage. A no-op on Windows (write-through moves).
bool syncDirectory(const std::filesystem::path& directory, std::string* error = nullptr);

#ifdef _WIN32
// MoveFileExW with replace and write-through. Retries briefly on transient sharing violations
// (antivirus, indexer, cloud sync). On failure errorCode is the last Win32 error.
bool moveFileReplacing(const std::filesystem::path& source, const std::filesystem::path& destination,
                       unsigned long& errorCode);
#endif

}  // namespace inventatory
