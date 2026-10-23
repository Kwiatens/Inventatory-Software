// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace inventatory {

// Request limits of the vendor APIs, and a process-wide count of the requests sent to each.
// The counts warn the user before a limit is reached and say when a reached limit ends.
// Limits are the vendors' published figures; see docs/vendor-rate-limits.md.

enum class Vendor { DigiKey, Mouser, Tme };

inline constexpr int kVendorCount = 3;

struct RateWindowSpec {
  const char* label;     // "per minute", "per day"
  std::int64_t seconds;  // Window length
  unsigned limit;        // Requests allowed in the window
};

struct VendorSpec {
  Vendor vendor;
  const char* name;     // "DigiKey"; also the transport service name
  const char* source;   // Where the limit is published
  std::vector<RateWindowSpec> windows;  // Empty when the vendor publishes no limit
};

// True for the error text limitMessage produces, whichever vendor it names. Callers stop a batch on it.
bool isVendorLimitMessage(const std::string& error);

const VendorSpec& vendorSpec(Vendor vendor);
std::optional<Vendor> vendorFromService(const std::string& service);

// Usage in one window.
struct WindowUsage {
  std::string label;
  std::int64_t seconds = 0;
  unsigned limit = 0;
  unsigned used = 0;
  bool full = false;
  bool warn = false;                // At or past the warning share, not yet full. Long windows only.
  std::int64_t freeAt = 0;          // Epoch seconds when one more request fits; 0 when not full
};

struct VendorUsageSnapshot {
  std::string name;
  std::vector<WindowUsage> windows;
  unsigned requestsLastDay = 0;     // Counted even when the vendor publishes no limit
  std::int64_t availableAt = 0;     // Epoch seconds the vendor is usable again; 0 when usable now
};

// A message for the user, queued when a warning or a limit is first noticed.
struct VendorNotice {
  bool limitReached = false;        // False: a "close to the limit" warning
  std::string text;
};

class VendorRateLimits {
 public:
  // Warn when a long window is this share used.
  static constexpr unsigned kWarnPercent = 80;
  // Windows at least this long are warned about. Shorter ones recover by waiting.
  static constexpr std::int64_t kWarnMinimumWindowSeconds = 600;
  // A wait up to this long is slept through; a longer one is refused with a message.
  static constexpr std::int64_t kMaximumWaitSeconds = 120;

  static VendorRateLimits& instance();

  // Loads counts from path and saves them there after every change. An empty path keeps the counts in
  // memory only. Safe to call again; the later call replaces the earlier counts.
  void setStoragePath(const std::filesystem::path& path);

  // Called before each HTTP request to a vendor. Waits if the wait is short, counts the request and
  // returns true. Returns false, with the message in *refusal, when the vendor is out of requests for
  // longer than kMaximumWaitSeconds. Nothing is sent then.
  bool admit(Vendor vendor, std::string* refusal = nullptr);

  // What a response told us. Fields are -1 when the vendor sent no such header.
  struct Response {
    std::uint32_t status = 0;
    std::int64_t retryAfterSeconds = -1;
    std::int64_t dailyLimit = -1;       // X-RateLimit-Limit
    std::int64_t dailyRemaining = -1;   // X-RateLimit-Remaining
    std::int64_t windowResetSeconds = -1;  // X-RateLimit-Reset or X-BurstLimit-Reset
  };
  void noteResponse(Vendor vendor, const Response& response);

  // Lower-level steps with the clock passed in. admit and noteResponse use them.
  void recordRequest(Vendor vendor, std::int64_t now);
  void recordLimitReached(Vendor vendor, std::int64_t now, std::optional<std::int64_t> retryAfterSeconds);
  void recordServerQuota(Vendor vendor, std::int64_t now, std::int64_t limit, std::int64_t remaining);
  // Seconds until a request may be sent; 0 when one may be sent now.
  std::int64_t waitSeconds(Vendor vendor, std::int64_t now);

  VendorUsageSnapshot snapshot(Vendor vendor, std::int64_t now) const;
  VendorUsageSnapshot snapshot(Vendor vendor) const;

  // "Mouser request limit reached; available again at 14:32", or "...; try again later" when unknown.
  // Always starts with "<name> request limit reached".
  std::string limitMessage(Vendor vendor, std::int64_t now) const;
  std::string limitMessage(Vendor vendor) const;

  // One line for Settings, e.g. "Mouser: 912 of 1000 per day, 3 of 30 per minute".
  static std::string usageLine(const VendorUsageSnapshot& snapshot);
  // The same without the vendor name, for a Settings row that is already labelled.
  static std::string usageSummary(const VendorUsageSnapshot& snapshot);

  // Drains the queued warnings and limit messages; the UI shows them.
  std::vector<VendorNotice> takeNotices();

  // Forgets every count and block. For tests.
  void reset();

  // "14:32" for today, "2026-10-24 14:32" for another day, in local time.
  static std::string formatTime(std::int64_t epochSeconds, std::int64_t now);

 private:
  VendorRateLimits() = default;

  struct VendorState {
    std::deque<std::int64_t> requests;  // Epoch seconds, oldest first
    std::int64_t blockedUntil = 0;      // The vendor said wait until then
    std::int64_t noticedBlock = 0;      // blockedUntil already announced
    std::vector<bool> warned;           // Per window: warning already queued
    // Last quota the vendor reported for its longest window.
    std::int64_t quotaLimit = -1;
    std::int64_t quotaRemaining = -1;
    std::int64_t quotaAt = 0;
  };

  void prune(VendorState& state, std::int64_t now) const;
  unsigned usedIn(const VendorState& state, const RateWindowSpec& window, std::int64_t now) const;
  std::int64_t freeAtFor(const VendorState& state, const RateWindowSpec& window, std::int64_t now) const;
  VendorUsageSnapshot snapshotLocked(Vendor vendor, std::int64_t now) const;
  std::int64_t waitSecondsLocked(Vendor vendor, std::int64_t now) const;
  std::int64_t availableAtLocked(Vendor vendor, std::int64_t now) const;
  std::string limitMessageLocked(Vendor vendor, std::int64_t now) const;
  void queueLimitNoticeLocked(Vendor vendor, std::int64_t now);
  void queueWarningsLocked(Vendor vendor, std::int64_t now);
  void loadLocked(std::int64_t now);
  void saveLocked() const;

  mutable std::mutex mutex_;
  std::array<VendorState, kVendorCount> states_;
  std::vector<VendorNotice> notices_;
  std::filesystem::path path_;
};

}  // namespace inventatory
