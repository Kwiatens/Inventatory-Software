// Inventatory - Rack label plans, ZPL, and summary helpers.

#include "label_printer/LabelPrinterPrivate.h"

#include "core/PartDescriptor.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <iomanip>
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
  return uppercaseAscii(fieldOrBlank(value, 20));
}

string rackLabelText(const string& code) {
  const auto number = rackNumberFromCode(code);
  if (number > 0) {
    ostringstream out;
    out << "RACK " << setw(2) << setfill('0') << number;
    return out.str();
  }
  const auto cleaned = trim(code);
  return cleaned.empty() ? "RACK" : fieldOrBlank("RACK " + cleaned, 12);
}

vector<string> rackCategoryLines(const string& category) {
  vector<string> words;
  istringstream input(trim(category));
  string word;
  while (input >> word) {
    words.push_back(word);
  }

  if (words.size() <= 1) {
    return {fieldOrBlank(category, 20)};
  }

  if (words.size() == 2) {
    return {fieldOrBlank(words[0], 14), fieldOrBlank(words[1], 14)};
  }

  const size_t splitAt = (words.size() + 1) / 2;
  vector<string> firstWords(words.begin(), words.begin() + splitAt);
  vector<string> secondWords(words.begin() + splitAt, words.end());
  vector<string> lines = {join(firstWords, ' '), join(secondWords, ' ')};
  for (auto& line : lines) {
    line = fieldOrBlank(line, 14);
  }
  return lines;
}

string rackCategoryFieldData(const vector<string>& lines) {
  if (lines.empty()) {
    return "RACK";
  }
  string output;
  for (const auto& line : lines) {
    if (!output.empty()) {
      output += "\\&";
    }
    output += sanitizeLabelText(line);
  }
  return output;
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
  plan.rackText = rackLabelText(rack.code);
  return plan;
}

string LabelPrinterService::buildRackLabelZpl(const InventatoryRack& rack) const {
  const auto plan = buildRackLabelPlan(rack);
  const auto categoryLines = rackCategoryLines(plan.categoryText);
  ostringstream out;
  out << "^XA\r\n";
  out << "^CI28\r\n";
  out << "^PW256\r\n";
  out << "^LL200\r\n";
  out << "^LH0,0\r\n";
  out << "^PR3\r\n";
  out << "^MD12\r\n";
  out << "\r\n";

  out << "^FX --- Black header bar ---\r\n";
  out << "^FO4,4^GB248,28,28,B,5^FS\r\n";
  out << "^FO12,11^A0N,16,16^FR^FDInventatory RACK^FS\r\n";
  out << "\r\n";

  out << "^FX --- Main category text ---\r\n";
  if (categoryLines.size() <= 1) {
    out << "^FO6,55^A0N,40,34^FB244,1,0,C^FD" << rackCategoryFieldData(categoryLines) << "^FS\r\n";
  } else {
    out << "^FO6,48^A0N,27,24^FB244,2,4,C^FD" << rackCategoryFieldData(categoryLines) << "^FS\r\n";
  }
  out << "\r\n";

  out << "^FX --- Thin separator under category ---\r\n";
  out << "^FO34,105^GB188,2,2^FS\r\n";
  out << "\r\n";

  out << "^FX --- Rack ID pill ---\r\n";
  out << "^FO43,128^GB170,42,42,B,5^FS\r\n";
  out << "^FO43,138^A0N,23,23^FR^FB170,1,0,C^FD" << sanitizeLabelText(plan.rackText) << "^FS\r\n";
  out << "\r\n";

  out << "^XZ\r\n";
  return out.str();
}

bool LabelPrinterService::printRackLabel(const InventatoryRack& rack, string* error) const {
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

  const auto zpl = buildRackLabelZpl(rack);
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
