// Inventatory - Settings appearance and color-picker rendering.

#include "App.h"
#include "ui/pages/settings/SettingsPagePrivate.h"

#include "ui/shared/AppUiShared.h"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace inventatory {

using namespace std;
using namespace settings_page_detail;

// The colour picker grid shown under the colour rows while it is open: twelve hues across, six
// brightness steps down, the current choice marked with a bracket pair.
ftxui::Elements App::renderSettingsAppearancePicker() const {
  auto self = const_cast<App*>(this);
  ftxui::Elements rows;
  for (int value = kAppearancePickerValueSteps - 1; value >= 0; --value) {
    ftxui::Elements pickerRow{ftxui::text(string(2 + 14, ' '))};
    for (int hue = 0; hue < kAppearancePickerHueSteps; ++hue) {
      const bool selected = hue == appearancePickerHue_ && value == appearancePickerValue_;
      const auto color = pickerColor(hue, value);
      auto cell = styledText(selected ? "[]" : "  ", uiCanvasBg(),
                             ftxui::Color::RGB(static_cast<uint8_t>((color >> 16) & 0xFFu),
                                               static_cast<uint8_t>((color >> 8) & 0xFFu),
                                               static_cast<uint8_t>(color & 0xFFu)));
      if (selected) cell = cell | ftxui::select;
      pickerRow.push_back(target(move(cell), "settings.appearance.picker." + to_string(hue) + "." + to_string(value),
                                 UiTargetKind::Cell,
                                 [self, hue, value] {
                                   self->appearancePickerHue_ = hue;
                                   self->appearancePickerValue_ = value;
                                   self->applyAppearancePickerColor();
                                 },
                                 true, false));
    }
    rows.push_back(ftxui::hbox(move(pickerRow)));
  }
  rows.push_back(ftxui::text(""));
  rows.push_back(ftxui::hbox({
      ftxui::text(string(2 + 14, ' ')),
      target(uiButton("Use colour", "Enter", UiButtonKind::Primary), "settings.appearance.picker.accept",
             UiTargetKind::Button, [self] { self->closeAppearancePicker(true); }),
      ftxui::text(" "),
      target(uiButton("Cancel", "Esc"), "settings.appearance.picker.cancel", UiTargetKind::Button,
             [self] { self->closeAppearancePicker(false); }),
  }));
  return rows;
}

}  // namespace inventatory
