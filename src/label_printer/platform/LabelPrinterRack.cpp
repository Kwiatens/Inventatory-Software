// Inventatory - Rack label plans, ZPL, and summary helpers.

#include "label_printer/core/LabelPrinterPrivate.h"

#include "core/parts/PartDescriptor.h"
#include "label_printer/layout/LabelPrinterZpl.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace inventatory {

using namespace std;

namespace label_printer_detail {

string makeJobName(const InventoryItem& item) {
  const auto id = trim(item.inventatoryId);
  if (!id.empty()) {
    return "Inventatory Label " + id;
  }
  if (!trim(item.partName).empty()) {
    return "Inventatory Label " + item.partName;
  }
  return "Inventatory Label";
}

string rackDisplayCategory(string value) {
  value = displayCategory(value);
  replace(value.begin(), value.end(), '-', ' ');
  replace(value.begin(), value.end(), '_', ' ');
  value = trim(value);
  if (value.empty()) {
    return "RACK";
  }
  return toUpper(fieldOrBlank(value, 20));
}

// "R12" -> "12", "R7" -> "07"; a code that is not an R-number prints as typed.
string rackNumberText(const string& code) {
  const auto number = rackNumberFromCode(code);
  if (number > 0) {
    ostringstream out;
    out << setw(2) << setfill('0') << number;
    return out.str();
  }
  const auto cleaned = trim(code);
  return cleaned.empty() ? string("RACK") : toUpper(fieldOrBlank(cleaned, 6));
}

// Friendlier short forms for the long built-in type names; custom types have none.
string rackShortCategory(const string& componentType) {
  const auto key = rackSymbolKey(componentType);
  if (key == "ic") return "ICs";
  if (key == "led") return "LEDs";
  if (key == "crystal") return "CRYSTALS";
  return {};
}

string makeRackJobName(const InventatoryRack& rack) {
  const auto code = trim(rack.code);
  return code.empty() ? "Inventatory Rack" : "Inventatory Rack " + code;
}

string partContextHeader(const InventoryItem& item) {
  return partShortDescription(item);
}

}  // namespace label_printer_detail

using namespace label_printer_detail;

InventatoryRackLabelPlan LabelPrinterService::buildRackLabelPlan(const InventatoryRack& rack) const {
  InventatoryRackLabelPlan plan;
  plan.categoryText = rackDisplayCategory(rack.componentType);
  plan.shortCategoryText = rackShortCategory(rack.componentType);
  plan.rackNumber = rackNumberText(rack.code);
  plan.symbolKey = rackSymbolKey(rack.componentType);
  return plan;
}

namespace {

// Rack label (layout A): the rack number is the largest thing on the label, white on a black block; the
// electrical symbol and the type name sit to its right.
constexpr int kBlockX = kLabelLeft;
constexpr int kBlockY = 28;
constexpr int kBlockWidth = 100;
constexpr int kBlockHeight = kLabelBottom - kBlockY;
constexpr int kRightCenterX = 179;
constexpr int kRightWidth = 120;
// A symbol never comes closer than this to the header bar or the type name.
constexpr int kSymbolClearance = 6;
constexpr int kNameTop = 116;
constexpr int kNameBottom = 176;
// A full type name is used when it fits on one line at this size or larger.
constexpr int kNiceNameSize = 22;

struct NameLayout {
  vector<string> lines;
  int size = 0;
};

// Splits a name over two lines at the word boundary that gives the largest size.
NameLayout twoLineName(const string& name, int maxWidth, initializer_list<int> sizes) {
  NameLayout best;
  istringstream words(name);
  vector<string> tokens{istream_iterator<string>(words), istream_iterator<string>()};
  for (size_t split = 1; split < tokens.size(); ++split) {
    const auto first = join(vector<string>(tokens.begin(), tokens.begin() + split), ' ');
    const auto second = join(vector<string>(tokens.begin() + split, tokens.end()), ' ');
    for (const auto size : sizes) {
      if (estimateFont0Width(first, size, size) > maxWidth || estimateFont0Width(second, size, size) > maxWidth) continue;
      if (size > best.size) best = {{first, second}, size};
      break;
    }
  }
  return best;
}

// The full name when it fits nicely on one line; otherwise the short form; otherwise the full name stepped down
// or split over two lines. Nothing is ever cut.
NameLayout chooseName(const InventatoryRackLabelPlan& plan, int maxWidth) {
  const auto full = sanitizeLabelText(plan.categoryText);
  const auto oneLine = [&](const string& text, initializer_list<int> sizes) {
    const auto fitted = fitFont0Text(text, maxWidth, sizes);
    return fitted.text == text ? NameLayout{{text}, fitted.size} : NameLayout{};
  };
  const auto nice = oneLine(full, {34, 30, 28, 26, 24, kNiceNameSize});
  if (nice.size >= kNiceNameSize) return nice;
  if (!plan.shortCategoryText.empty()) {
    const auto shortName = oneLine(sanitizeLabelText(plan.shortCategoryText), {34, 30, 28, 26, 24, 22, 20, 18});
    if (shortName.size > 0) return shortName;
  }
  const auto single = oneLine(full, {34, 30, 28, 26, 24, 22, 20, 18, 16, 14});
  const auto split = twoLineName(full, maxWidth, {30, 26, 24, 22, 20, 18, 16, 14});
  if (split.size > single.size + 3 || single.size == 0) {
    if (split.size > 0) return split;
  }
  if (single.size > 0) return single;
  const auto clipped = fitFont0Text(full, maxWidth, {14});
  return {{clipped.text}, clipped.size};
}

}  // namespace

string LabelPrinterService::buildRackLabelZpl(const InventatoryRack& rack, SymbolStandard standard) const {
  const auto plan = buildRackLabelPlan(rack);
  ostringstream out;
  out << "^XA\r\n";
  out << "^CI28\r\n";
  out << "^PW256\r\n";
  out << "^LL200\r\n";
  out << "^LH0,0\r\n";
  out << "^PR3\r\n";
  out << "^MD12\r\n";
  out << "\r\n";

  out << "^FX --- Header: logo and title ---\r\n";
  writeBrandHeader(out, "INVENTATORY RACK");
  out << "\r\n";

  out << "^FX --- Rack number block ---\r\n";
  writeBox(out, kBlockX, kBlockY, kBlockWidth, kBlockHeight, kBlockWidth, 1);
  const auto blockCenterX = kBlockX + kBlockWidth / 2;
  writeText(out, centeredLeft(blockCenterX, 0, estimateFont0Width("RACK", 16, 16)), 38, 16, 16, "RACK", "^FR");
  // The number takes the largest size that fits the block; three-digit racks step down, they are never cut.
  const auto numberSize = fitFont0Text(plan.rackNumber, kBlockWidth - 10, {112, 104, 96, 88, 80, 72, 64, 56, 48});
  const auto numberRegionTop = 62;
  const auto numberRegionBottom = kLabelBottom - 7;
  if (!numberSize.text.empty()) {
    const auto top = centeredTop((numberRegionTop + numberRegionBottom) / 2.0, numberSize.size);
    writeText(out, centeredLeft(blockCenterX, 0, numberSize.width), top, numberSize.size, numberSize.size,
              numberSize.text, "^FR");
  }
  out << "\r\n";

  const auto name = chooseName(plan, kRightWidth + 4);
  const auto pitch = static_cast<int>(name.size * 1.12);
  const auto total = pitch * static_cast<int>(name.lines.size() - 1) + name.size * kCapHeight;
  const auto firstTop = static_cast<int>((kNameTop + kNameBottom) / 2.0 - total / 2.0 + 0.5);

  out << "^FX --- Electrical symbol ---\r\n";
  // Centred in the space between the header bar and the type name, whatever the name's height.
  const auto& symbol = rackSymbolFor(rack.componentType, standard);
  const auto symbolTop = std::clamp(static_cast<int>((kHeaderHeight + firstTop - symbol.height) / 2.0 + 0.5),
                                    kHeaderHeight + kSymbolClearance,
                                    std::max(kHeaderHeight + kSymbolClearance, firstTop - kSymbolClearance - symbol.height));
  writeGraphic(out, kRightCenterX - symbol.width / 2, symbolTop, symbol);
  out << "\r\n";

  out << "^FX --- Type name ---\r\n";
  for (size_t index = 0; index < name.lines.size(); ++index) {
    const auto width = estimateFont0Width(name.lines[index], name.size, name.size);
    writeText(out, centeredLeft(kRightCenterX, 0, width), firstTop + static_cast<int>(index) * pitch, name.size,
              name.size, name.lines[index]);
  }

  out << "^XZ\r\n";
  return out.str();
}

bool LabelPrinterService::printRackLabel(const InventatoryRack& rack, string* error, SymbolStandard standard) const {
  if (backend_ == nullptr) {
    if (error != nullptr) {
      *error = "Printer backend unavailable";
    }
    return false;
  }

  if (!hasConfiguredPrinter()) {
    if (error != nullptr) {
      *error = "No printer configured";
    }
    return false;
  }

  const auto zpl = buildRackLabelZpl(rack, standard);
  return backend_->sendRawJob(configuredPrinter_, makeRackJobName(rack), zpl, error);
}

string LabelPrinterService::summaryText() const {
  if (!hasConfiguredPrinter()) {
    return "Printer: not configured";
  }

  const auto info = configuredPrinterInfo();
  if (!info) {
    return "Printer: " + configuredPrinter_ + " [missing]";
  }

  ostringstream out;
  out << "Printer: " << info->name << " [" << info->statusText << "]";
  return out.str();
}

string sanitizeLabelText(const string& value) {
  return sanitiseZplFragment(value);
}

}  // namespace inventatory
