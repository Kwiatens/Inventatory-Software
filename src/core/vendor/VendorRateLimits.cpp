// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#include "core/vendor/VendorRateLimits.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace inventatory {
namespace {

using std::int64_t;
using std::string;

constexpr int64_t kMinute = 60;
constexpr int64_t kDay = 24 * 60 * 60;
constexpr int64_t kMaximumBlockSeconds = 7 * kDay;
constexpr std::size_t kMaximumStoredRequests = 20000;
constexpr const char* kStorageHeader = "inventatory-vendor-usage 1";

const std::vector<VendorSpec>& specs() {
  // DigiKey: developer.digikey.com/documentation, Product Information product.
  // Mouser: mouser.com/api-search (Search API). TME publishes no figure.
  static const std::vector<VendorSpec> table = {
      {Vendor::DigiKey, "DigiKey", "developer.digikey.com/documentation",
       {{"per minute", kMinute, 120}, {"per day", kDay, 1000}}},
      {Vendor::Mouser, "Mouser", "mouser.com/api-search", {{"per minute", kMinute, 30}, {"per day", kDay, 1000}}},
      {Vendor::Tme, "TME", "no published limit", {}},
  };
  return table;
}

int64_t clockNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

size_t indexOf(Vendor vendor) { return static_cast<size_t>(vendor); }

string lower(string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool localTime(int64_t seconds, std::tm& out) {
  const std::time_t value = static_cast<std::time_t>(seconds);
#ifdef _WIN32
  return localtime_s(&out, &value) == 0;
#else
  return localtime_r(&value, &out) != nullptr;
#endif
}

}  // namespace

const VendorSpec& vendorSpec(Vendor vendor) { return specs()[indexOf(vendor)]; }

std::optional<Vendor> vendorFromService(const string& service) {
  const auto wanted = lower(service);
  for (const auto& spec : specs()) {
    if (lower(spec.name) == wanted) return spec.vendor;
  }
  return std::nullopt;
}

bool isVendorLimitMessage(const string& error) {
  for (const auto& spec : specs()) {
    if (error.rfind(string(spec.name) + " request limit reached", 0) == 0) return true;
  }
  return false;
}

VendorRateLimits& VendorRateLimits::instance() {
  static VendorRateLimits limits;
  return limits;
}

string VendorRateLimits::formatTime(int64_t epochSeconds, int64_t now) {
  std::tm then{};
  std::tm today{};
  if (!localTime(epochSeconds, then) || !localTime(now, today)) return "later";
  char buffer[32];
  const bool sameDay = then.tm_year == today.tm_year && then.tm_yday == today.tm_yday;
  const char* format = sameDay ? "%H:%M" : "%Y-%m-%d %H:%M";
  if (std::strftime(buffer, sizeof(buffer), format, &then) == 0) return "later";
  return buffer;
}

void VendorRateLimits::prune(VendorState& state, int64_t now) const {
  // Keep a day of history: enough for every window, and for the "last 24 hours" figure.
  const int64_t horizon = now - kDay;
  while (!state.requests.empty() && state.requests.front() <= horizon) state.requests.pop_front();
  // A clock that moved back leaves entries in the future; they would never expire.
  while (!state.requests.empty() && state.requests.back() > now + kMinute) state.requests.pop_back();
  while (state.requests.size() > kMaximumStoredRequests) state.requests.pop_front();
}

unsigned VendorRateLimits::usedIn(const VendorState& state, const RateWindowSpec& window, int64_t now) const {
  const int64_t since = now - window.seconds;
  const auto first = std::upper_bound(state.requests.begin(), state.requests.end(), since);
  auto used = static_cast<int64_t>(state.requests.end() - first);
  // The vendor's own count covers requests from other copies of the app sharing the key.
  if (state.quotaLimit == static_cast<int64_t>(window.limit) && state.quotaRemaining >= 0 &&
      state.quotaAt > since) {
    const auto after = std::upper_bound(state.requests.begin(), state.requests.end(), state.quotaAt);
    const int64_t reported = state.quotaLimit - state.quotaRemaining + static_cast<int64_t>(state.requests.end() - after);
    used = std::max(used, reported);
  }
  return static_cast<unsigned>(std::max<int64_t>(0, used));
}

// When one more request fits in a full window. The request that brings the count under the limit
// is (used - limit) entries from the oldest one inside the window.
int64_t VendorRateLimits::freeAtFor(const VendorState& state, const RateWindowSpec& window, int64_t now) const {
  const int64_t since = now - window.seconds;
  std::vector<int64_t> inWindow;
  for (const auto at : state.requests) {
    if (at > since) inWindow.push_back(at);
  }
  const size_t index = usedIn(state, window, now) - window.limit;
  if (index < inWindow.size()) return inWindow[index] + window.seconds;
  // Full by the vendor's count, which this copy cannot see inside: assume the whole window.
  return (inWindow.empty() ? now : inWindow.back()) + window.seconds;
}

VendorUsageSnapshot VendorRateLimits::snapshotLocked(Vendor vendor, int64_t now) const {
  const auto& spec = vendorSpec(vendor);
  const auto& state = states_[indexOf(vendor)];
  VendorUsageSnapshot out;
  out.name = spec.name;
  out.requestsLastDay = static_cast<unsigned>(std::count_if(
      state.requests.begin(), state.requests.end(), [&](int64_t at) { return at > now - kDay; }));
  for (const auto& window : spec.windows) {
    WindowUsage usage;
    usage.label = window.label;
    usage.seconds = window.seconds;
    usage.limit = window.limit;
    usage.used = usedIn(state, window, now);
    usage.full = usage.used >= window.limit;
    usage.warn = !usage.full && window.seconds >= kWarnMinimumWindowSeconds &&
                 static_cast<std::uint64_t>(usage.used) * 100U >= static_cast<std::uint64_t>(window.limit) * kWarnPercent;
    if (usage.full) usage.freeAt = freeAtFor(state, window, now);
    out.windows.push_back(usage);
  }
  out.availableAt = availableAtLocked(vendor, now);
  return out;
}

int64_t VendorRateLimits::availableAtLocked(Vendor vendor, int64_t now) const {
  const auto& state = states_[indexOf(vendor)];
  int64_t at = state.blockedUntil > now ? state.blockedUntil : 0;
  const auto& spec = vendorSpec(vendor);
  for (const auto& window : spec.windows) {
    if (usedIn(state, window, now) >= window.limit) at = std::max(at, freeAtFor(state, window, now));
  }
  return at > now ? at : 0;
}

int64_t VendorRateLimits::waitSecondsLocked(Vendor vendor, int64_t now) const {
  const auto at = availableAtLocked(vendor, now);
  return at > now ? at - now : 0;
}

string VendorRateLimits::limitMessageLocked(Vendor vendor, int64_t now) const {
  const auto& spec = vendorSpec(vendor);
  const auto at = availableAtLocked(vendor, now);
  string text = string(spec.name) + " request limit reached";
  if (at == 0) return text + "; try again later";
  // Name the window that holds the vendor back, when it is one we count.
  const auto snap = snapshotLocked(vendor, now);
  for (const auto& window : snap.windows) {
    if (window.full && window.freeAt == at) {
      text += " (" + std::to_string(window.limit) + " " + window.label + ")";
      break;
    }
  }
  return text + "; available again at " + formatTime(at, now);
}

void VendorRateLimits::queueLimitNoticeLocked(Vendor vendor, int64_t now) {
  auto& state = states_[indexOf(vendor)];
  const auto at = availableAtLocked(vendor, now);
  if (at == 0 || at == state.noticedBlock) return;
  state.noticedBlock = at;
  notices_.push_back({true, limitMessageLocked(vendor, now) + "."});
}

void VendorRateLimits::queueWarningsLocked(Vendor vendor, int64_t now) {
  const auto& spec = vendorSpec(vendor);
  auto& state = states_[indexOf(vendor)];
  if (state.warned.size() != spec.windows.size()) state.warned.assign(spec.windows.size(), false);
  const auto snap = snapshotLocked(vendor, now);
  for (size_t i = 0; i < snap.windows.size(); ++i) {
    const auto& window = snap.windows[i];
    if (window.seconds < kWarnMinimumWindowSeconds) continue;
    if (window.warn) {
      if (state.warned[i]) continue;
      state.warned[i] = true;
      notices_.push_back({false, snap.name + ": " + std::to_string(window.used) + " of " + std::to_string(window.limit) +
                                     " requests used " + window.label + ". The limit is close; further lookups will be refused once it is reached."});
    } else if (!window.full) {
      state.warned[i] = false;
    }
  }
}

void VendorRateLimits::recordRequest(Vendor vendor, int64_t now) {
  const std::lock_guard<std::mutex> lock(mutex_);
  auto& state = states_[indexOf(vendor)];
  prune(state, now);
  state.requests.push_back(now);
  queueWarningsLocked(vendor, now);
  saveLocked();
}

void VendorRateLimits::recordServerQuota(Vendor vendor, int64_t now, int64_t limit, int64_t remaining) {
  const std::lock_guard<std::mutex> lock(mutex_);
  auto& state = states_[indexOf(vendor)];
  state.quotaLimit = limit;
  state.quotaRemaining = remaining;
  state.quotaAt = now;
  queueWarningsLocked(vendor, now);
}

void VendorRateLimits::recordLimitReached(Vendor vendor, int64_t now, std::optional<int64_t> retryAfterSeconds) {
  const std::lock_guard<std::mutex> lock(mutex_);
  auto& state = states_[indexOf(vendor)];
  int64_t block = 0;
  if (retryAfterSeconds.has_value() && *retryAfterSeconds > 0) {
    block = std::min(*retryAfterSeconds, kMaximumBlockSeconds);
  } else {
    // No hint: assume the longest window that is nearly used up, else the shortest window.
    const auto snap = snapshotLocked(vendor, now);
    const WindowUsage* nearly = nullptr;
    for (const auto& window : snap.windows) {
      if (window.limit > 0 && static_cast<std::uint64_t>(window.used) * 10U >= static_cast<std::uint64_t>(window.limit) * 9U &&
          (nearly == nullptr || window.seconds > nearly->seconds)) {
        nearly = &window;
      }
    }
    if (nearly != nullptr) {
      block = nearly->freeAt > now ? nearly->freeAt - now : nearly->seconds;
    } else {
      block = snap.windows.empty() ? kMinute : snap.windows.front().seconds;
    }
  }
  state.blockedUntil = std::max(state.blockedUntil, now + block);
  // A short block is waited out by admit; only a long one is worth telling the user about.
  if (state.blockedUntil - now > kMaximumWaitSeconds) queueLimitNoticeLocked(vendor, now);
  saveLocked();
}

int64_t VendorRateLimits::waitSeconds(Vendor vendor, int64_t now) {
  const std::lock_guard<std::mutex> lock(mutex_);
  prune(states_[indexOf(vendor)], now);
  return waitSecondsLocked(vendor, now);
}

bool VendorRateLimits::admit(Vendor vendor, string* refusal) {
  for (int attempt = 0; attempt < 5; ++attempt) {
    int64_t wait = 0;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      const auto now = clockNow();
      auto& state = states_[indexOf(vendor)];
      prune(state, now);
      wait = waitSecondsLocked(vendor, now);
      if (wait == 0) {
        state.requests.push_back(now);
        queueWarningsLocked(vendor, now);
        saveLocked();
        return true;
      }
      if (wait > kMaximumWaitSeconds) {
        queueLimitNoticeLocked(vendor, now);
        if (refusal != nullptr) *refusal = limitMessageLocked(vendor, now);
        return false;
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(wait));
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  if (refusal != nullptr) *refusal = limitMessageLocked(vendor, clockNow());
  return false;
}

void VendorRateLimits::noteResponse(Vendor vendor, const Response& response) {
  const auto now = clockNow();
  if (response.dailyLimit > 0 && response.dailyRemaining >= 0) {
    recordServerQuota(vendor, now, response.dailyLimit, response.dailyRemaining);
  }
  if (response.status == 429) {
    std::optional<int64_t> wait;
    if (response.retryAfterSeconds > 0) wait = response.retryAfterSeconds;
    else if (response.windowResetSeconds > 0) wait = response.windowResetSeconds;
    recordLimitReached(vendor, now, wait);
  }
}

VendorUsageSnapshot VendorRateLimits::snapshot(Vendor vendor, int64_t now) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return snapshotLocked(vendor, now);
}

VendorUsageSnapshot VendorRateLimits::snapshot(Vendor vendor) const { return snapshot(vendor, clockNow()); }

string VendorRateLimits::limitMessage(Vendor vendor, int64_t now) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return limitMessageLocked(vendor, now);
}

string VendorRateLimits::limitMessage(Vendor vendor) const { return limitMessage(vendor, clockNow()); }

string VendorRateLimits::usageLine(const VendorUsageSnapshot& snapshot) {
  return snapshot.name + ": " + usageSummary(snapshot);
}

string VendorRateLimits::usageSummary(const VendorUsageSnapshot& snapshot) {
  string text;
  if (snapshot.windows.empty()) {
    text += std::to_string(snapshot.requestsLastDay) + " requests in the last 24 hours";
  } else {
    // Longest window first: the daily figure matters most.
    auto windows = snapshot.windows;
    std::sort(windows.begin(), windows.end(), [](const WindowUsage& a, const WindowUsage& b) { return a.seconds > b.seconds; });
    for (size_t i = 0; i < windows.size(); ++i) {
      if (i > 0) text += ", ";
      text += std::to_string(windows[i].used) + " of " + std::to_string(windows[i].limit) + " " + windows[i].label;
    }
  }
  if (snapshot.availableAt > 0) {
    text += ". Limit reached; available again at " + formatTime(snapshot.availableAt, clockNow());
  }
  return text;
}

std::vector<VendorNotice> VendorRateLimits::takeNotices() {
  const std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(notices_, {});
}

void VendorRateLimits::reset() {
  const std::lock_guard<std::mutex> lock(mutex_);
  for (auto& state : states_) state = VendorState{};
  notices_.clear();
  path_.clear();
}

void VendorRateLimits::setStoragePath(const std::filesystem::path& path) {
  const std::lock_guard<std::mutex> lock(mutex_);
  for (auto& state : states_) state = VendorState{};
  path_ = path;
  loadLocked(clockNow());
}

// Format: a header line, then "<vendor> blocked <epoch>" and "<vendor> requests <epoch> <epoch> ...".
void VendorRateLimits::loadLocked(int64_t now) {
  if (path_.empty()) return;
  std::ifstream in(path_);
  string line;
  if (!in || !std::getline(in, line) || line != kStorageHeader) return;
  while (std::getline(in, line)) {
    std::istringstream words(line);
    string vendorName;
    string kind;
    if (!(words >> vendorName >> kind)) continue;
    const auto vendor = vendorFromService(vendorName);
    if (!vendor.has_value()) continue;
    auto& state = states_[indexOf(*vendor)];
    if (kind == "blocked") {
      int64_t until = 0;
      if (words >> until && until > now && until - now <= kMaximumBlockSeconds) {
        state.blockedUntil = until;
        // Loaded blocks were announced in an earlier session.
        state.noticedBlock = until;
      }
    } else if (kind == "requests") {
      int64_t at = 0;
      while (words >> at) {
        if (at > now - kDay && at <= now + kMinute) state.requests.push_back(at);
      }
      std::sort(state.requests.begin(), state.requests.end());
      prune(state, now);
    }
  }
  // Windows already past the warning share at start-up were warned about before.
  for (int i = 0; i < kVendorCount; ++i) {
    const auto vendor = static_cast<Vendor>(i);
    const auto snap = snapshotLocked(vendor, now);
    states_[i].warned.assign(snap.windows.size(), false);
    for (size_t w = 0; w < snap.windows.size(); ++w) states_[i].warned[w] = snap.windows[w].warn || snap.windows[w].full;
  }
}

void VendorRateLimits::saveLocked() const {
  if (path_.empty()) return;
  auto temporary = path_;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) return;
    out << kStorageHeader << '\n';
    for (const auto& spec : specs()) {
      const auto& state = states_[indexOf(spec.vendor)];
      const auto name = lower(spec.name);
      if (state.blockedUntil > 0) out << name << " blocked " << state.blockedUntil << '\n';
      if (!state.requests.empty()) {
        out << name << " requests";
        for (const auto at : state.requests) out << ' ' << at;
        out << '\n';
      }
    }
    if (!out) return;
  }
  // Usage counts are advisory: a failed save only loses the count after a restart.
  std::error_code ignored;
  std::filesystem::rename(temporary, path_, ignored);
  if (ignored) std::filesystem::remove(temporary, ignored);
}

}  // namespace inventatory
