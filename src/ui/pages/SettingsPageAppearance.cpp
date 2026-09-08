// Inventatory - Settings appearance and color-picker rendering.

#include "App.h"
#include "ui/pages/SettingsPagePrivate.h"

#include "ui/shared/AppUiShared.h"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace inventatory {

using namespace std;
using namespace settings_page_detail;

ftxui::Elements App::renderSettingsAppearanceRows(int contentWidth) const {
  auto self = const_cast<App*>(this);
  ftxui::Elements rows;
    const int colorCellWidth = max(28, (contentWidth - 2) / 2);
    const auto addAppearanceSection = [&](const string& title,
                                          initializer_list<AppearanceColorRole> roles) {
      rows.push_back(uiHeaderText(title, uiSecondaryText()));
      vector<AppearanceColorRole> section(roles);
      for (size_t offset = 0; offset < section.size(); offset += 2) {
        ftxui::Elements columns;
        for (size_t column = 0; column < 2; ++column) {
          const auto roleIndex = offset + column;
          if (roleIndex >= section.size()) {
            columns.push_back(ftxui::filler() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, colorCellWidth));
            continue;
          }
          const auto role = section[roleIndex];
          const auto index = static_cast<int>(role);
          columns.push_back(target(
              appearanceColorLine(role, colorCellWidth, settingsField_ == index),
              "settings.appearance.color." + to_string(index), UiTargetKind::Field,
              [self, index] {
                self->settingsField_ = index;
                self->settingsEditingField_ = false;
                self->appearancePickerOpen_ = false;
                self->inputBuffer_.clear();
                self->dirty_ = true;
              }));
        }
        rows.push_back(ftxui::hbox(move(columns)));
      }
    };

    if (!appearancePickerOpen_) {
      addAppearanceSection("BACKGROUNDS", {
          AppearanceColorRole::CanvasBg,
          AppearanceColorRole::SurfaceBg,
          AppearanceColorRole::RaisedSurfaceBg,
          AppearanceColorRole::HoverBg,
          AppearanceColorRole::SelectionBg,
          AppearanceColorRole::Divider,
      });
      addAppearanceSection("TEXT", {
          AppearanceColorRole::PrimaryText,
          AppearanceColorRole::SecondaryText,
          AppearanceColorRole::MutedText,
          AppearanceColorRole::FocusText,
      });
      addAppearanceSection("ACCENTS AND STATUS", {
          AppearanceColorRole::Interactive,
          AppearanceColorRole::Success,
          AppearanceColorRole::Link,
          AppearanceColorRole::WarningText,
          AppearanceColorRole::DangerText,
      });
      addAppearanceSection("STATUS BACKGROUNDS", {
          AppearanceColorRole::ActiveBg,
          AppearanceColorRole::ActiveSoftBg,
          AppearanceColorRole::WarningBg,
          AppearanceColorRole::DangerBg,
          AppearanceColorRole::DangerFlashBg,
      });
    }

    const auto selectedIndex = clamp(settingsField_, 0, static_cast<int>(kAppearanceColorCount) - 1);
    const auto selectedRole = static_cast<AppearanceColorRole>(selectedIndex);
    rows.push_back(uiDivider());
    rows.push_back(uiHeaderText("EDIT COLOR", uiSecondaryText()));
    rows.push_back(ftxui::hbox({
        styledText(" Selected", uiSecondaryText()) |
            ftxui::size(ftxui::WIDTH, ftxui::EQUAL, settingsLabelWidth(contentWidth)),
        styledText(appearanceColorLabel(selectedRole), uiPrimaryText()),
        ftxui::filler(),
        styledText("   ", uiPrimaryText(), uiAppearanceColor(selectedRole)),
        styledText(" " + appearanceColorHex(settingsDraft_.appearance.colors[static_cast<size_t>(selectedIndex)]),
                   uiPrimaryText()),
        styledText(" "),
    }));

    const auto hexValue = settingsEditingField_ && settingsField_ == selectedIndex
                              ? inputBuffer_ + "_"
                              : appearanceColorHex(settingsDraft_.appearance.colors[static_cast<size_t>(selectedIndex)]);
    rows.push_back(target(settingLine("Hex value", hexValue, contentWidth,
                                      settingsEditingField_ && settingsField_ == selectedIndex),
                          "settings.appearance.hex", UiTargetKind::Field,
                          [self] { self->beginSettingsFieldEdit(self->settingsField_); }));

    if (appearancePickerOpen_) {
      rows.push_back(uiHeaderText("HUE / VALUE PICKER", uiSecondaryText()));
      for (int value = kAppearancePickerValueSteps - 1; value >= 0; --value) {
        ftxui::Elements pickerRow;
        for (int hue = 0; hue < kAppearancePickerHueSteps; ++hue) {
          const bool selected = hue == appearancePickerHue_ && value == appearancePickerValue_;
          const auto color = pickerColor(hue, value);
          auto cell = styledText(selected ? "[]" : "  ", uiPrimaryText(),
                                 ftxui::Color::RGB(static_cast<uint8_t>((color >> 16) & 0xFFu),
                                                   static_cast<uint8_t>((color >> 8) & 0xFFu),
                                                   static_cast<uint8_t>(color & 0xFFu)));
          pickerRow.push_back(target(move(cell), "settings.appearance.picker." + to_string(hue) + "." +
                                                       to_string(value),
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
      rows.push_back(styledText("Arrows change hue/value   Enter accept   Esc cancel", uiMutedText()));
    }

    ftxui::Elements appearanceActions;
    if (appearancePickerOpen_) {
      appearanceActions.push_back(target(uiPrimaryButton("Accept picker"), "settings.appearance.picker.accept",
                                         UiTargetKind::Button, [self] { self->closeAppearancePicker(true); }));
      appearanceActions.push_back(ftxui::text("  "));
      appearanceActions.push_back(target(uiSecondaryButton("Cancel picker", uiSecondaryText()),
                                         "settings.appearance.picker.cancel", UiTargetKind::Button,
                                         [self] { self->closeAppearancePicker(false); }));
    } else {
      appearanceActions.push_back(target(uiPrimaryButton("Open picker"), "settings.appearance.picker.open",
                                         UiTargetKind::Button, [self] { self->openAppearancePicker(); }));
      appearanceActions.push_back(ftxui::text("  "));
      appearanceActions.push_back(target(uiSecondaryButton("Edit hex", uiSecondaryText()),
                                         "settings.appearance.hex.edit", UiTargetKind::Button,
                                         [self] { self->beginSettingsFieldEdit(self->settingsField_); }));
    }
    appearanceActions.push_back(ftxui::text("  "));
    appearanceActions.push_back(target(uiSecondaryButton("Reset selected", uiWarnColor()),
                                       "settings.appearance.reset.selected", UiTargetKind::Button,
                                       [self] { self->resetSelectedAppearanceColor(); }));
    appearanceActions.push_back(ftxui::text("  "));
    appearanceActions.push_back(target(uiSecondaryButton("Reset all", uiDangerColor()),
                                       "settings.appearance.reset.all", UiTargetKind::Button,
                                       [self] { self->resetAppearanceColors(); }));
    rows.push_back(ftxui::hbox(move(appearanceActions)));
  return rows;
}

}  // namespace inventatory
