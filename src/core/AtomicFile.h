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
bool writeFileAtomically(const std::filesystem::path& destination, std::string_view contents,
                         std::string* error = nullptr);

}  // namespace inventatory
