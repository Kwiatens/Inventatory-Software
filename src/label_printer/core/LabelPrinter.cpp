// Inventatory - Item and wire label plans, ZPL, and dispatch.

#include "label_printer/core/LabelPrinterPrivate.h"

#include "core/inventory/InventoryInternals.h"
#include "ui/shared/AppUiShared.h"

#include <sstream>
#include <utility>

namespace inventatory {

using namespace std;
using namespace label_printer_detail;

InventatoryLabelPlan LabelPrinterService::buildLabelPlan(const InventoryItem& item, string rackLocation) const {
  InventatoryLabelPlan plan;
  const auto parameterLines = parameterLinesForItem(item);
  plan.categoryHeader = partContextHeader(item);
  plan.mainValue = mainLabelValue(item);
  plan.packageLine = shortPackageLine(item);
  plan.manufacturerLine = manufacturerLine(item);
  if (!parameterLines.empty()) {
    plan.parameterLine1 = parameterLines[0];
  }
  if (parameterLines.size() > 1) {
    plan.parameterLine2 = parameterLines[1];
  }
  if (parameterLines.size() > 2) {
    plan.parameterLine3 = parameterLines[2];
  }
  plan.inventatoryId = trim(item.inventatoryId);
  plan.scannerHint = buildVisibleInventatoryId(item);
  if (trim(plan.scannerHint).empty()) {
    plan.scannerHint = shortCode(plan.inventatoryId, 16);
  }
  plan.barcodeHint = normalizeMachineCode(item.machineCode);
  plan.rackLocation = trim(rackLocation);

  return plan;
}

string LabelPrinterService::buildZpl(const InventoryItem& item, string rackLocation) const {
  const auto plan = buildLabelPlan(item, move(rackLocation));
  const auto categoryHeader = sanitiseZplFragment(plan.categoryHeader);
  const auto mainValue = sanitiseZplFragment(plan.mainValue);
  const auto categoryHeaderFont = SingleLineFont{17, 17};
  // Preserve the original large title treatment. Only longer titles step
  // down, rather than shrinking all label typography to fit every case.
  const auto mainValueLength = mainValue.size();
  const auto mainValueFont = isCompactManufacturerPartNumber(mainValue) || mainValueLength <= 14
                                 ? SingleLineFont{34, 31}
                                 : mainValueLength <= 18 ? SingleLineFont{30, 25}
                                 : mainValueLength <= 22 ? SingleLineFont{26, 21}
                                 : mainValueLength <= 26 ? SingleLineFont{22, 17}
                                                         : SingleLineFont{18, 14};
  const auto packageLine = fitSingleLineLabel(plan.packageLine, 24);
  const auto manufacturerLine = fitSingleLineLabel(plan.manufacturerLine, 20);
  const auto parameterLine1 = fitSingleLineLabel(plan.parameterLine1, 24);
  const auto parameterLine2 = fitSingleLineLabel(plan.parameterLine2, 24);
  const auto parameterLine3 = fitSingleLineLabel(plan.parameterLine3, 24);
  const auto barcodeHint = fitSingleLineLabel(plan.barcodeHint, 14);
  const auto rackHint = fitSingleLineLabel(plan.rackLocation, 12);
  ostringstream out;
  out << "^XA\r\n";
  out << "^CI28\r\n";
  out << "^PW256\r\n";
  out << "^LL200\r\n";
  out << "^LH0,0\r\n";
  out << "^PR3\r\n";
  out << "^MD12\r\n";
  out << "\r\n";

  out << "^FX --- Header ---\r\n";
  // The bar uses equal ten-dot margins on both sides of the 256-dot label.
  out << "^FO10,0^GB236,24,24,B,6^FS\r\n";
  out << "^FO12,6^A0N," << categoryHeaderFont.height << ',' << categoryHeaderFont.width << "^FR^FD"
      << sanitizeLabelText(categoryHeader) << "^FS\r\n";
  out << "\r\n";

  out << "^FX --- Main value ---\r\n";
  out << "^FO10,33^A0N," << mainValueFont.height << ',' << mainValueFont.width << "^FD"
      << sanitizeLabelText(mainValue) << "^FS\r\n";
  out << "\r\n";

  out << "^FX --- Package ---\r\n";
  if (!packageLine.empty()) {
    out << "^FO10,70^A0N,14,14^FD" << sanitizeLabelText(packageLine) << "^FS\r\n";
  }
  out << "\r\n";

  out << "^FX --- Thin divider ---\r\n";
  out << "^FO10,93^GB146,1,1^FS\r\n";
  out << "\r\n";

  out << "^FX --- Manufacturer / parameters ---\r\n";
  if (!manufacturerLine.empty()) {
    out << "^FO10,100^A0N,16,16^FD" << sanitizeLabelText(manufacturerLine) << "^FS\r\n";
  }
  if (!parameterLine1.empty()) {
    out << "^FO10,118^A0N,15,15^FD" << sanitizeLabelText(parameterLine1) << "^FS\r\n";
  }
  if (!parameterLine2.empty()) {
    out << "^FO10,136^A0N,15,15^FD" << sanitizeLabelText(parameterLine2) << "^FS\r\n";
  }
  if (!parameterLine3.empty()) {
    out << "^FO10,154^A0N,15,15^FD" << sanitizeLabelText(parameterLine3) << "^FS\r\n";
  }
  out << "\r\n";

  out << "^FX --- QR code ---\r\n";
  // Keep the QR symbol compact so it wraps less on curved labels.
  out << "^FO170,60^BQN,2,3^FDLA," << sanitizeLabelText(barcodeHint) << "^FS\r\n";
  out << "\r\n";

  if (!rackHint.empty()) {
    out << "^FX --- Inventatory rack location ---\r\n";
    out << "^FO10,173^A0N,18,18^FD" << sanitizeLabelText(rackHint) << "^FS\r\n";
  }

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
