// Inventatory - primary workspace navigation contract.

#pragma once

#include <array>
#include <cstdint>

namespace inventatory::app_navigation {

enum class PrimaryPage : std::uint8_t {
  Stock,
  Racks,
  Import,
  Projects,
  History,
  Settings,
};

struct PrimaryNavigationEntry {
  PrimaryPage page;
  char shortcut;
  const char* id;
  const char* label;
};

inline constexpr std::array<PrimaryNavigationEntry, 6> kPrimaryNavigationEntries = {{
    {PrimaryPage::Stock, '1', "stock", "Stock"},
    {PrimaryPage::Racks, '2', "racks", "Racks"},
    {PrimaryPage::Import, '3', "import", "Import"},
    {PrimaryPage::Projects, '4', "projects", "Projects"},
    {PrimaryPage::History, '5', "history", "History"},
    {PrimaryPage::Settings, '6', "settings", "Settings"},
}};

inline constexpr const std::array<PrimaryNavigationEntry, 6>& primaryNavigationEntries() {
  return kPrimaryNavigationEntries;
}

// Supported terminal size. Anything smaller shows the resize notice and ignores input, so keys
// and clicks cannot act on a screen the user cannot see.
inline constexpr int kMinimumTerminalColumns = 100;
inline constexpr int kMinimumTerminalRows = 30;

inline constexpr bool terminalTooSmall(int columns, int rows) {
  return columns < kMinimumTerminalColumns || rows < kMinimumTerminalRows;
}

// The header row holds the brand, the six destinations, the always-visible
// "Actions - Space" control and, only when there is room left over, the clock.
// At the 100-column minimum the clock would push the Actions control off the
// row, so it yields first: full date and time, then HH:MM, then nothing.
enum class HeaderClock : std::uint8_t { Full, Compact, Hidden };

inline constexpr int kHeaderClockFullColumns = 21;     // " 2026-10-06 12:00:00 "
inline constexpr int kHeaderClockCompactColumns = 7;   // " 12:00 "

inline constexpr HeaderClock headerClockFor(int screenWidth, int fixedColumns) {
  const int spare = screenWidth - fixedColumns;
  if (spare >= kHeaderClockFullColumns) return HeaderClock::Full;
  if (spare >= kHeaderClockCompactColumns) return HeaderClock::Compact;
  return HeaderClock::Hidden;
}

inline constexpr const PrimaryNavigationEntry* primaryNavigationEntryForShortcut(char shortcut) {
  for (const auto& entry : kPrimaryNavigationEntries) {
    if (entry.shortcut == shortcut) return &entry;
  }
  return nullptr;
}

}  // namespace inventatory::app_navigation
