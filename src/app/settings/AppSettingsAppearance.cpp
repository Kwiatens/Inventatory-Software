// Inventatory - Appearance settings formatting and parsing.

#include "app/settings/AppSettings.h"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace inventatory {

using namespace std;

namespace {

int hexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

}  // namespace

const char* appearanceColorKey(AppearanceColorRole role) {
  switch (role) {
    case AppearanceColorRole::CanvasBg: return "canvas_bg";
    case AppearanceColorRole::SurfaceBg: return "surface_bg";
    case AppearanceColorRole::RaisedSurfaceBg: return "raised_surface_bg";
    case AppearanceColorRole::HoverBg: return "hover_bg";
    case AppearanceColorRole::SelectionBg: return "selection_bg";
    case AppearanceColorRole::Divider: return "divider";
    case AppearanceColorRole::PrimaryText: return "primary_text";
    case AppearanceColorRole::SecondaryText: return "secondary_text";
    case AppearanceColorRole::MutedText: return "muted_text";
    case AppearanceColorRole::FocusText: return "focus_text";
    case AppearanceColorRole::Interactive: return "interactive";
    case AppearanceColorRole::Success: return "success";
    case AppearanceColorRole::Link: return "link";
    case AppearanceColorRole::WarningText: return "warning_text";
    case AppearanceColorRole::DangerText: return "danger_text";
    case AppearanceColorRole::ActiveBg: return "active_bg";
    case AppearanceColorRole::ActiveSoftBg: return "active_soft_bg";
    case AppearanceColorRole::WarningBg: return "warning_bg";
    case AppearanceColorRole::DangerBg: return "danger_bg";
    case AppearanceColorRole::DangerFlashBg: return "danger_flash_bg";
    case AppearanceColorRole::Count: break;
  }
  return "unknown";
}

const char* appearanceColorLabel(AppearanceColorRole role) {
  switch (role) {
    case AppearanceColorRole::CanvasBg: return "Canvas background";
    case AppearanceColorRole::SurfaceBg: return "Surface background";
    case AppearanceColorRole::RaisedSurfaceBg: return "Raised surface";
    case AppearanceColorRole::HoverBg: return "Hover background";
    case AppearanceColorRole::SelectionBg: return "Selection background";
    case AppearanceColorRole::Divider: return "Divider";
    case AppearanceColorRole::PrimaryText: return "Primary text";
    case AppearanceColorRole::SecondaryText: return "Secondary text";
    case AppearanceColorRole::MutedText: return "Muted text";
    case AppearanceColorRole::FocusText: return "Focus text";
    case AppearanceColorRole::Interactive: return "Interactive / accent";
    case AppearanceColorRole::Success: return "Success";
    case AppearanceColorRole::Link: return "Link";
    case AppearanceColorRole::WarningText: return "Warning text";
    case AppearanceColorRole::DangerText: return "Danger text";
    case AppearanceColorRole::ActiveBg: return "Active background";
    case AppearanceColorRole::ActiveSoftBg: return "Active soft background";
    case AppearanceColorRole::WarningBg: return "Warning background";
    case AppearanceColorRole::DangerBg: return "Danger background";
    case AppearanceColorRole::DangerFlashBg: return "Danger flash background";
    case AppearanceColorRole::Count: break;
  }
  return "Unknown";
}

string appearanceColorHex(uint32_t rgb) {
  ostringstream output;
  output << '#' << uppercase << hex << setw(6) << setfill('0') << (rgb & 0xFFFFFFu);
  return output.str();
}

bool parseAppearanceColorHex(const string& text, uint32_t& rgb) {
  string value;
  for (const char character : text) {
    if (!isspace(static_cast<unsigned char>(character))) value.push_back(character);
  }
  if (!value.empty() && value.front() == '#') value.erase(value.begin());
  if (value.size() != 6) return false;

  uint32_t parsed = 0;
  for (const char character : value) {
    const int digit = hexDigit(character);
    if (digit < 0) return false;
    parsed = (parsed << 4) | static_cast<uint32_t>(digit);
  }
  rgb = parsed;
  return true;
}


}  // namespace inventatory
