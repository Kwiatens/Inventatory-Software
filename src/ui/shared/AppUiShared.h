// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Shared terminal UI formatting and inventory detail helpers.

#pragma once

#include "app/settings/AppSettings.h"
#include "core/inventory/Inventory.h"
#include "core/inventory/ParameterLookup.h"
#include "core/text/Voice.h"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <cstdint>
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

// Semantic palette. It is process-local, so settings pages can stage a replacement
// without every render function taking a palette parameter.
void applyUiAppearance(const AppearanceSettings& appearance);
const AppearanceSettings& activeUiAppearance();
ftxui::Color uiAppearanceColor(AppearanceColorRole role);

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
ftxui::Color uiActiveBg();
ftxui::Color uiActiveSoftBg();
ftxui::Color uiWarningBg();
ftxui::Color uiDangerBg();
ftxui::Color uiMutedColor();
ftxui::Color uiDimColor();
ftxui::Color uiPanelLeftBg();
ftxui::Color uiPanelRightBg();
ftxui::Color uiRowSelectedBg();

ftxui::Element styledText(const string& text, optional<ftxui::Color> fg = nullopt,
                          optional<ftxui::Color> bg = nullopt);
// Headers and section labels are bold; body text is normal weight.
ftxui::Element uiHeaderText(const string& text, optional<ftxui::Color> fg = nullopt,
                            optional<ftxui::Color> bg = nullopt);
ftxui::Element uiBodyText(const string& text, optional<ftxui::Color> fg = nullopt,
                          optional<ftxui::Color> bg = nullopt);
ftxui::Element uiSectionHeader(const string& text, optional<ftxui::Color> fg = nullopt,
                               optional<ftxui::Color> bg = nullopt);
ftxui::Element fullLine(const string& text, optional<ftxui::Color> fg = nullopt,
                        optional<ftxui::Color> bg = nullopt);
// A table cell exactly `width` columns wide: text clipped with an ellipsis, left or right aligned.
ftxui::Element fixedCell(const string& text, int width, ftxui::Color color, bool rightAlign = false);
ftxui::Element centered(ftxui::Element element);
// The timed delete confirmation drawn over a page. noun names what is deleted ("part", "rack");
// Enter confirms once secondsLeft reaches zero.
ftxui::Element uiDeleteConfirmation(ftxui::Element page, int screenWidth, const string& title, const string& noun,
                                    const string& selected, int secondsLeft);
ftxui::Element panel(const string& title, ftxui::Elements body, optional<ftxui::Color> titleColor = nullopt,
                     optional<ftxui::Color> borderColor = nullopt);
ftxui::Element footerField(const string& title, const string& body, ftxui::Color titleColor, ftxui::Color bodyColor,
                           ftxui::Color background, bool flashing = false);

// uiPrimaryButton is the filled cyan control for a panel's one main action;
// uiSecondaryButton is the ordinary raised control for other actions.
ftxui::Element uiPrimaryButton(const string& label, bool enabled = true);
ftxui::Element uiSecondaryButton(const string& label, optional<ftxui::Color> fg = nullopt, bool enabled = true);

// Look per docs/ui-style-guide.md. A danger button changes only its text colour.
enum class UiButtonKind { Normal, Primary, Danger };
ftxui::Element uiButton(const string& label, const string& key = {}, UiButtonKind kind = UiButtonKind::Normal);

// One plain-sentence voice line. `maxWidth` > 0 cuts it at that many cells with an
// ellipsis, since an overflowing row would squeeze every word.
ftxui::Element uiVoiceLine(const VoiceLine& line, optional<ftxui::Color> bg = nullopt, int maxWidth = 0);

// Two-line page header. `right` holds the pending state and the page's buttons.
ftxui::Element uiPageHeader(ftxui::Element title, ftxui::Element sub, ftxui::Element right = nullptr);

// Milliseconds since start, for sub-second animation. time(nullptr) is too coarse.
long long uiAnimationTicks();
bool uiBlinkOn(int periodMs = 900);

bool uiBoxContains(const ftxui::Box& box, int x, int y);

string displayCategory(const string& category);
// Display width in cells (wide glyphs 2, combining marks 0). Use this, not size(), for columns.
size_t displayWidth(const string& value);
// Longest prefix of `value` that fits in `cells` columns, cut on a glyph boundary.
string takeCells(const string& value, size_t cells);
// Cuts on glyph boundaries, never inside a UTF-8 sequence. `maxLength` is in cells.
string ellipsize(const string& value, size_t maxLength);
// A progress bar of `cells` ASCII cells, "[####------]", filled by done / total. It is drawn again
// with each new count, never animated, so the number beside it is what changes.
string uiProgressBar(size_t done, size_t total, size_t cells = 10);
// Plain bytes for a size, such as "640 KB" or "1.5 MB".
string formatBytes(uint64_t bytes);

// One line of progress for a long transfer in the bottom message row: a label, then a bar with its
// percentage when `hasBar` is set, then a muted detail.
struct UiTransferProgress {
  string label;
  bool hasBar = false;
  uint64_t done = 0;
  uint64_t total = 0;
  string detail;
};
ftxui::Element uiTransferProgressRow(const UiTransferProgress& progress);

// Splits a part name ("CAP CER 0.1UF 16V X7R 0603") into type, first value and spec.
// The pieces rejoin to the original words; a name with no value keeps all in `value`.
struct PartNameParts {
  string type;
  string value;
  string spec;
};
PartNameParts splitPartName(const string& name);

// Danger when out, warning when low, otherwise primary.
ftxui::Color uiQuantityColor(const InventoryItem& item, int lowStockThreshold);

// Dim type, bold value, dim spec. `maxWidth` > 0 truncates the end, keeping the value visible.
ftxui::Element uiPartName(const string& name, int maxWidth = 0, bool selected = false);
// Word-wraps on spaces; a word wider than the line is split on glyph boundaries.
vector<string> wrapText(const string& text, int width);

string joinTags(const vector<string>& tags);
string renderTags(const vector<string>& tags);
string renderUrl(const string& url);

bool categoryContains(const InventoryItem& item, initializer_list<const char*> needles);
string prettyLabel(const string& label);

vector<DetailField> electricalFieldsForItem(const InventoryItem& item);
vector<DetailField> stockPreviewFields(const InventoryItem& item, string rack = {});
vector<DetailField> detailCoreFields(const InventoryItem& item, string rack = {});
ftxui::Element detailFieldLine(const DetailField& field, int width);

}  // namespace inventatory

