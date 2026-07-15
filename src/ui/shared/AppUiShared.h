// Inventatory - Hardware Inventory Management System
// Shared terminal UI formatting and inventory detail helpers.

#pragma once

#include "core/Inventory.h"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace inventatory {

using std::filesystem::path;
using std::initializer_list;
using std::nullopt;
using std::optional;
using std::ostringstream;
using std::size_t;
using std::string;
using std::vector;

struct DetailField {
  string label;
  string value;
  ftxui::Color labelColor;
  ftxui::Color valueColor;
};

// Graphite / Turquoise semantic palette. New UI code should use these role
// names; the legacy helpers below remain as compatibility aliases during the
// workspace migration.
ftxui::Color uiCanvasBg();
ftxui::Color uiSurfaceBg();
ftxui::Color uiRaisedSurfaceBg();
ftxui::Color uiHoverBg();
ftxui::Color uiSelectionBg();
ftxui::Color uiDividerColor();
ftxui::Element uiDivider();
ftxui::Color uiPrimaryText();
ftxui::Color uiSecondaryText();
ftxui::Color uiMutedText();
ftxui::Color uiInteractiveColor();
ftxui::Color uiFocusColor();

ftxui::Color uiTitleColor();
ftxui::Color uiAccentColor();
ftxui::Color uiInfoColor();
ftxui::Color uiSuccessColor();
ftxui::Color uiLinkColor();
ftxui::Color uiLabelColor();
ftxui::Color uiWarnColor();
ftxui::Color uiDangerColor();
ftxui::Color uiMutedColor();
ftxui::Color uiDimColor();
ftxui::Color uiPanelLeftBg();
ftxui::Color uiPanelRightBg();
ftxui::Color uiRowDarkBg();
ftxui::Color uiRowLightBg();
ftxui::Color uiRowSelectedBg();

ftxui::Element styledText(const string& text, optional<ftxui::Color> fg = nullopt,
                          optional<ftxui::Color> bg = nullopt);
ftxui::Element fullLine(const string& text, optional<ftxui::Color> fg = nullopt,
                        optional<ftxui::Color> bg = nullopt);
ftxui::Element bulletLine(const string& label, const string& value, ftxui::Color labelColor,
                          ftxui::Color valueColor);
ftxui::Element panel(const string& title, ftxui::Elements body, optional<ftxui::Color> titleColor = nullopt,
                     optional<ftxui::Color> borderColor = nullopt);
ftxui::Element footerField(const string& title, const string& body, ftxui::Color titleColor, ftxui::Color bodyColor,
                           ftxui::Color background, bool flashing = false);
ftxui::Element statusCueChip(const string& label, bool active, bool flashing, ftxui::Color fg,
                             ftxui::Color activeBg, ftxui::Color flashingBg, ftxui::Color inactiveBg);
ftxui::Element quantityBadge(int quantity, bool selected = false);

// A compact status indicator: a colored dot, a muted label, and an optional
// trailing value (e.g. statusDot("device", green, "12s")). Used in the header.
ftxui::Element statusDot(const string& label, ftxui::Color color, const string& value = {});
bool uiBoxContains(const ftxui::Box& box, int x, int y);

string displayCategory(const string& category);
string ellipsize(const string& value, size_t maxLength);
vector<string> wrapText(const string& text, int width);

string joinTags(const vector<string>& tags);
string renderTags(const vector<string>& tags);
string renderParameters(const vector<Parameter>& parameters);
string renderUrl(const string& url);

string normalizeKey(string value);
bool categoryContains(const InventoryItem& item, initializer_list<const char*> needles);
bool parameterLabelMatches(const string& lhs, const string& rhs);
bool looksLikePackagingValue(const string& value);
const Parameter* findParameter(const vector<Parameter>& parameters, initializer_list<const char*> names);
optional<string> parameterValue(const InventoryItem& item, initializer_list<const char*> names);
string prettyLabel(const string& label);

vector<DetailField> electricalFieldsForItem(const InventoryItem& item);
vector<DetailField> stockPreviewFields(const InventoryItem& item, string rack = {});
vector<DetailField> detailCoreFields(const InventoryItem& item, string rack = {});
ftxui::Element detailFieldLine(const DetailField& field, int width);

}  // namespace inventatory

