// Inventatory - Hardware Inventory Management System
// Crash-resistant replacement for small local text files.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace inventatory {

// Writes bytes to a uniquely named temporary file in the destination
// directory, flushes and closes it, then replaces the destination.  The
// destination is never truncated before the replacement succeeds.  On
// failure, only the temporary file created by this invocation is removed.
// The operation is deliberately non-throwing so callers can retain their
// in-memory state and report a useful retryable error.
//
// On Linux the replacement is durable once this returns: the temporary file is
// fsync'd before the rename and the containing directory afterwards, and
// interrupted system calls are retried. A destination that is a symbolic link
// (for example a dotfile-managed settings file) is written through to its
// target, so the link itself survives.
bool writeFileAtomically(const std::filesystem::path& destination, std::string_view contents,
                         std::string* error = nullptr);

// Flushes the contents of an existing file to stable storage. Used for files
// that are copied or written in place and then published by a rename.
bool syncFile(const std::filesystem::path& file, std::string* error = nullptr);

// Flushes a directory's entries (a file created or renamed inside it) to stable
// storage. Windows renames are made durable by write-through moves instead, so
// this is a successful no-op there.
bool syncDirectory(const std::filesystem::path& directory, std::string* error = nullptr);

#ifdef _WIN32
// Moves source over destination (MoveFileExW, replace-existing, write-through) and retries a few
// times, with a short pause, when the failure is a transient access or sharing violation caused by an
// antivirus scan, the search indexer or cloud sync briefly holding the destination. On failure
// errorCode is the last Win32 error.
bool moveFileReplacing(const std::filesystem::path& source, const std::filesystem::path& destination,
                       unsigned long& errorCode);
#endif

}  // namespace inventatory
