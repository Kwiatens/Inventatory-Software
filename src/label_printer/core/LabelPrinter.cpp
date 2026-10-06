// Inventatory - Item and wire label plans, ZPL, and dispatch.

#include "label_printer/core/LabelPrinterPrivate.h"

#include "core/inventory/InventoryInternals.h"
#include "label_printer/layout/LabelPrinterZpl.h"
#include "ui/shared/AppUiShared.h"

#include <iterator>
#include <sstream>
#include <utility>

namespace inventatory {

using namespace std;
using namespace label_printer_detail;

InventatoryLabelPlan LabelPrinterService::buildLabelPlan(const InventoryItem& item, string rackLocation) const {
  InventatoryLabelPlan plan;
  plan.categoryHeader = partContextHeader(item);
  plan.mainValue = mainLabelValue(item);
  plan.mainIsMeasuredValue = isMeasuredValueItem(item);
  plan.mainTolerance = mainLabelTolerance(item);
  plan.packageLine = shortPackageLine(item);
  plan.manufacturerLine = manufacturerLine(item);
  plan.parameters = parameterTilesForItem(item);
  plan.inventatoryId = trim(item.inventatoryId);
  plan.scannerHint = buildVisibleInventatoryId(item);
  if (trim(plan.scannerHint).empty()) {
    plan.scannerHint = shortCode(plan.inventatoryId, 16);
  }
  plan.barcodeHint = normalizeMachineCode(item.machineCode);
  plan.rackLocation = trim(rackLocation);
  splitRackLocation(plan.rackLocation, plan.rackCode, plan.rackCell);

  return plan;
}

namespace {

// Item label layout on the 256 x 200 dot (32 x 25 mm, 203 dpi) stock. The
// left column says what the part is; the right column holds the QR code,
// the readable ID, the rack slot and a pen field for recording a move.
//
// Test prints land about 3 dots right of and 10 dots below ^FO, so the
// layout starts at x 7 / y 0 and ends at x 243 / y 179: that leaves an even
// margin of about 10 dots on all four sides of the printed label.
constexpr int kBottom = kLabelBottom;
constexpr int kHeaderY = 0;
constexpr int kPillY = 80;
constexpr int kPillHeight = 20;
constexpr int kDividerY = 106;
constexpr int kTileY = 110;
constexpr int kTileRowPitch = 39;
// ^BQ draws the symbol about 10 dots below its field origin; at magnification
// 3 a 21 module symbol is 63 dots square.
constexpr int kQrFieldY = 18;
constexpr int kQrSize = 63;
constexpr int kQrBottom = kQrFieldY + 10 + kQrSize;
constexpr int kIdY = kQrBottom + 3;
constexpr int kSlotY = kIdY + 17;
constexpr int kSlotHeight = 23;
constexpr int kPenCaptionY = kSlotY + kSlotHeight + 3;
constexpr int kPenY = kPenCaptionY + 12;
constexpr int kLeft = kLabelLeft;
constexpr int kLeftColumnWidth = 164;
constexpr int kRight = 180;
constexpr int kRightColumnWidth = kQrSize;
constexpr int kSlotSplit = 40;
// The parameter grid: two columns of kLabelTileWidth around a hairline rule.
constexpr int kTileColumnPitch = 86;
constexpr int kTileRuleX = kLeft + 81;
// The value sits centred between the header bar and the package row.
constexpr double kValueCenterY = (kHeaderY + kHeaderHeight + kPillY) / 2.0;
constexpr char kPlusMinus[] = "\xC2\xB1";

// One line of text centred in the left column and on centerY.
void writeCenteredLine(ostringstream& out, double centerY, int size, const string& text) {
  writeText(out, centeredLeft(kLeft, kLeftColumnWidth, estimateFont0Width(text, size, size)), centeredTop(centerY, size),
            size, size, text);
}

void writeMainValue(ostringstream& out, const InventatoryLabelPlan& plan) {
  if (plan.mainIsMeasuredValue) {
    string number;
    string unit;
    splitMeasuredValue(plan.mainValue, number, unit);
    const auto& tolerance = plan.mainTolerance;
    constexpr int toleranceSize = 18;
    constexpr int toleranceGap = 7;
    for (const auto size : {46, 42, 38, 34}) {
      const auto unitSize = scaledDots(size, 0.72);
      const auto numberWidth = estimateFont0Width(number, size, size);
      const auto unitWidth = unit.empty() ? 0 : estimateFont0Width(unit, unitSize, unitSize) + 1;
      const auto toleranceWidth =
          tolerance.empty() ? 0 : estimateFont0Width(tolerance, toleranceSize, toleranceSize) + toleranceGap;
      const auto groupWidth = numberWidth + unitWidth + toleranceWidth;
      if (groupWidth > kLeftColumnWidth) continue;
      // Number, unit and tolerance share one baseline; the group is centred.
      const auto x = centeredLeft(kLeft, kLeftColumnWidth, groupWidth);
      const auto y = centeredTop(kValueCenterY, size);
      writeText(out, x, y, size, size, number);
      if (!unit.empty()) {
        writeText(out, x + numberWidth + 1, y + scaledDots(size - unitSize, kCapHeight), unitSize, unitSize, unit);
      }
      if (!tolerance.empty()) {
        const auto toleranceX = x + numberWidth + unitWidth + toleranceGap;
        const auto toleranceY = y + scaledDots(size - toleranceSize, kCapHeight);
        // The font draws "±" about 3 dots below the digits' centre line.
        if (tolerance.rfind(kPlusMinus, 0) == 0) {
          const auto signWidth = estimateFont0Width(kPlusMinus, toleranceSize, toleranceSize);
          writeText(out, toleranceX, toleranceY - 3, toleranceSize, toleranceSize, kPlusMinus);
          writeText(out, toleranceX + signWidth, toleranceY, toleranceSize, toleranceSize,
                    tolerance.substr(sizeof(kPlusMinus) - 1));
        } else {
          writeText(out, toleranceX, toleranceY, toleranceSize, toleranceSize, tolerance);
        }
      }
      return;
    }
  }
  const auto oneLine = fitFont0Text(plan.mainValue, kLeftColumnWidth, {40, 36, 32, 28, 26});
  if (!oneLine.text.empty() && oneLine.text == sanitizeLabelText(plan.mainValue)) {
    writeCenteredLine(out, kValueCenterY, oneLine.size, oneLine.text);
    return;
  }
  // Long names wrap onto two lines at word boundaries before anything is cut.
  istringstream words(sanitizeLabelText(plan.mainValue));
  vector<string> tokens{istream_iterator<string>(words), istream_iterator<string>()};
  for (const auto size : {24, 22, 20, 18}) {
    string first;
    string second;
    for (const auto& token : tokens) {
      auto& line = second.empty() && estimateFont0Width(first.empty() ? token : first + " " + token, size, size) <=
                                         kLeftColumnWidth
                       ? first
                       : second;
      line += (line.empty() ? "" : " ") + token;
    }
    if (!first.empty() && !second.empty() && estimateFont0Width(second, size, size) <= kLeftColumnWidth) {
      const auto pitch = size + 2;
      const auto blockTop = kValueCenterY - (pitch + size * kCapHeight) / 2;
      writeText(out, centeredLeft(kLeft, kLeftColumnWidth, estimateFont0Width(first, size, size)),
                static_cast<int>(blockTop + 0.5), size, size, first);
      writeText(out, centeredLeft(kLeft, kLeftColumnWidth, estimateFont0Width(second, size, size)),
                static_cast<int>(blockTop + 0.5) + pitch, size, size, second);
      return;
    }
  }
  // A single long token (a part number) steps down in size rather than wrapping.
  const auto fitted = fitFont0Text(plan.mainValue, kLeftColumnWidth, {24, 22, 20, 18, 16, 14});
  if (!fitted.text.empty()) {
    writeCenteredLine(out, kValueCenterY, fitted.size, fitted.text);
  }
}

void writeParameterTiles(ostringstream& out, const vector<LabelParameterTile>& tiles) {
  const auto count = min<size_t>(tiles.size(), 4);
  for (size_t index = 0; index < count; ++index) {
    const auto x = kLeft + static_cast<int>(index % 2) * kTileColumnPitch;
    const auto y = kTileY + static_cast<int>(index / 2) * kTileRowPitch;
    const auto caption = fitFont0Text(toUpper(tiles[index].caption), kLabelTileWidth, {12, 11, 10});
    if (!caption.text.empty()) {
      writeText(out, x, y, caption.size, caption.size == 12 ? 11 : caption.size, caption.text);
    }
    // Values were checked to fit when the tile was chosen; they are never clipped.
    const auto value = fitFont0Text(tiles[index].value, kLabelTileWidth, {22, 20, 18, 16, 14, kLabelTileMinSize});
    if (!value.text.empty()) {
      // A leading minus has a wide left bearing: pull it back to the text edge.
      const auto bearing = value.text.front() == '-' ? 2 : 0;
      writeText(out, x - bearing, y + 13 + scaledDots(22 - value.size, kCapHeight), value.size, value.size, value.text);
    }
  }
  if (count > 1) {
    writeBox(out, kTileRuleX, kTileY + 2, 1, count > 2 ? kTileRowPitch + 33 : 33, 1);
  }
}

void writeSlot(ostringstream& out, const InventatoryLabelPlan& plan) {
  const bool hasSlot = !plan.rackCode.empty();
  // Without a slot there is no chip: the pen field moves up and takes its place.
  const auto captionY = hasSlot ? kPenCaptionY : kSlotY;
  const auto penY = hasSlot ? kPenY : kSlotY + 12;
  if (hasSlot) {
    const bool hasCell = !plan.rackCell.empty();
    const auto rackWidth = hasCell ? kSlotSplit : kRightColumnWidth;
    const auto textCenterY = kSlotY + kSlotHeight / 2.0;
    writeBox(out, kRight, kSlotY, kRightColumnWidth, kSlotHeight, 2, 3);
    writeBox(out, kRight, kSlotY, rackWidth, kSlotHeight, kSlotHeight, 3);
    const auto rack = fitFont0Text(plan.rackCode, rackWidth - 5, {17, 16, 15, 14, 13});
    writeText(out, centeredLeft(kRight, rackWidth, rack.width), centeredTop(textCenterY, rack.size), rack.size, rack.size,
              rack.text, "^FR");
    if (hasCell) {
      const auto cellWidth = kRightColumnWidth - kSlotSplit;
      const auto cell = fitFont0Text(plan.rackCell, cellWidth - 2, {19, 17, 15, 13});
      writeText(out, centeredLeft(kRight + kSlotSplit, cellWidth, cell.width), centeredTop(textCenterY, cell.size),
                cell.size, cell.size, cell.text);
    }
  }

  // Pen field: corner brackets that stay open so handwriting is not boxed in.
  constexpr int arm = 7;
  constexpr int stroke = 2;
  const auto penHeight = kBottom - penY;
  writeText(out, kRight, captionY, 11, 11, hasSlot ? "NEW SLOT" : "SLOT");
  const auto right = kRight + kRightColumnWidth;
  const auto bottom = penY + penHeight;
  for (const auto x : {kRight, right - arm}) {
    writeBox(out, x, penY, arm, stroke, stroke);
    writeBox(out, x, bottom - stroke, arm, stroke, stroke);
  }
  for (const auto x : {kRight, right - stroke}) {
    writeBox(out, x, penY, stroke, arm, stroke);
    writeBox(out, x, bottom - arm, stroke, arm, stroke);
  }
}

}  // namespace

string LabelPrinterService::buildZpl(const InventoryItem& item, string rackLocation) const {
  const auto plan = buildLabelPlan(item, move(rackLocation));
  ostringstream out;
  out << "^XA\r\n";
  out << "^CI28\r\n";
  out << "^PW256\r\n";
  out << "^LL200\r\n";
  out << "^LH0,0\r\n";
  out << "^PR3\r\n";
  out << "^MD12\r\n";
  out << "\r\n";

  out << "^FX --- Header: logo and category ---\r\n";
  writeBrandHeader(out, toUpper(plan.categoryHeader));
  out << "\r\n";

  out << "^FX --- Main value ---\r\n";
  writeMainValue(out, plan);
  out << "\r\n";

  out << "^FX --- Package and manufacturer ---\r\n";
  const auto pillCenterY = kPillY + kPillHeight / 2.0;
  auto manufacturerX = kLeft;
  if (!plan.packageLine.empty()) {
    const auto package = fitFont0Text(plan.packageLine, 90, {16, 14, 12});
    const auto pillWidth = package.width + 10;
    writeBox(out, kLeft, kPillY, pillWidth, kPillHeight, kPillHeight, 4);
    writeText(out, kLeft + 5, centeredTop(pillCenterY, package.size), package.size, package.size, package.text, "^FR");
    manufacturerX = kLeft + pillWidth + 6;
  }
  if (!plan.manufacturerLine.empty()) {
    // The full name when it fits; otherwise drop corporate suffixes
    // ("Infineon Technologies" -> "Infineon") before anything is cut.
    const auto available = kLeft + kLeftColumnWidth - manufacturerX;
    const auto candidates = manufacturerCandidates(plan.manufacturerLine);
    FittedLabelText manufacturer;
    for (const auto& candidate : candidates) {
      manufacturer = fitFont0Text(candidate, available, {16, 15, 14, 13, 12});
      if (manufacturer.text == sanitizeLabelText(candidate)) break;
    }
    if (!manufacturer.text.empty()) {
      // Mixed-case text has no capitals on most of its line: sit it 1 dot lower.
      writeText(out, manufacturerX, centeredTop(pillCenterY + 1, manufacturer.size), manufacturer.size,
                manufacturer.size, manufacturer.text);
    }
  }
  out << "\r\n";

  out << "^FX --- Parameters ---\r\n";
  writeBox(out, kLeft, kDividerY, kLeftColumnWidth - 4, 1, 1);
  writeParameterTiles(out, plan.parameters);
  out << "\r\n";

  out << "^FX --- QR code and readable ID ---\r\n";
  // Keep the QR symbol compact so it wraps less on curved labels.
  out << "^FO" << kRight << ',' << kQrFieldY << "^BQN,2,3^FDLA," << sanitizeLabelText(plan.barcodeHint) << "^FS\r\n";
  if (!plan.scannerHint.empty()) {
    const auto id = fitFont0Text(plan.scannerHint, kRightColumnWidth, {14, 13, 12});
    writeText(out, centeredLeft(kRight, kQrSize, id.width), kIdY, id.size, id.size, id.text);
  }
  out << "\r\n";

  out << "^FX --- Rack slot and pen field ---\r\n";
  writeSlot(out, plan);

  out << "^XZ\r\n";
  return out.str();
}

bool LabelPrinterService::printItemLabel(const InventoryItem& item, string* error, string rackLocation) const {
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

  const auto zpl = buildZpl(item, move(rackLocation));
  return backend_->sendRawJob(configuredPrinter_, makeJobName(item), zpl, error);
}

string LabelPrinterService::buildWireLabelZpl(const string& text) const {
  const auto label = fitSingleLineLabel(text, 18);
  if (label.empty()) return {};
  const auto font = cableFlagFont(label);
  ostringstream out;
  out << "^XA\r\n";
  out << "^CI28\r\n^PW256\r\n^LL200\r\n^LH0,0\r\n^PR3\r\n^MD12\r\n";
  out << "^FX --- Cable flag: normal-facing half ---\r\n";
  out << "^FO10,12^A0N," << font.height << ',' << font.width << "^FB236,1,0,C^FD"
      << sanitizeLabelText(label) << "^FS\r\n";
  out << "^FO14,88^GB228,2,2^FS\r\n";
  out << "^FX --- Cable flag: lower half, matching orientation ---\r\n";
  out << "^FO10,112^A0N," << font.height << ',' << font.width << "^FB236,1,0,C^FD"
      << sanitizeLabelText(label) << "^FS\r\n";
  out << "^FO14,88^GB228,2,2^FS\r\n";
  out << "^XZ\r\n";
  return out.str();
}

bool LabelPrinterService::printWireLabel(const string& text, string* error) const {
  if (backend_ == nullptr) {
    if (error != nullptr) *error = "Printer backend unavailable";
    return false;
  }
  if (!hasConfiguredPrinter()) {
    if (error != nullptr) *error = "No printer configured";
    return false;
  }
  const auto zpl = buildWireLabelZpl(text);
  if (zpl.empty()) {
    if (error != nullptr) *error = "Wire label text is empty";
    return false;
  }
  return backend_->sendRawJob(configuredPrinter_, "Inventatory Wire Label", zpl, error);
}

}  // namespace inventatory
