// Inventatory - Shared HTTP server implementation limits.

#pragma once

#include <cstddef>

namespace inventatory::http_server_detail {

inline constexpr std::size_t kMaxHttpHeaderBytes = 8U * 1024U;
inline constexpr std::size_t kMaxHttpBodyBytes = 64U * 1024U;

}  // namespace inventatory::http_server_detail
