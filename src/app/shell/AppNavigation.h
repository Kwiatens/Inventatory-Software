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

inline constexpr const PrimaryNavigationEntry* primaryNavigationEntryForShortcut(char shortcut) {
  for (const auto& entry : kPrimaryNavigationEntries) {
    if (entry.shortcut == shortcut) return &entry;
  }
  return nullptr;
}

}  // namespace inventatory::app_navigation
