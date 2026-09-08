// Inventatory - Label text and electrical-value formatting helpers.

#include "label_printer/LabelPrinterPrivate.h"

#include "core/InventoryInternals.h"
#include "core/PartDescriptor.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <regex>
#include <sstream>
#include <system_error>

namespace inventatory {

using namespace std;

namespace label_printer_detail {

string uppercaseAscii(string value) {
  transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(toupper(ch));
  });
  return value;
}

string lowerAscii(string value) {
  transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(tolower(ch));
  });
  return value;
}

string shortCode(const string& value, size_t maxLength) {
  return ellipsize(trim(value), maxLength);
}

string fieldOrBlank(const string& value, size_t maxLength);
string sanitiseZplFragment(const string& value);

string shortParameterLabel(const string& label) {
  const auto key = normalizeKey(label);
  if (key.empty()) {
    return {};
  }

  if (key.find("capacitance") != string::npos || key == "value") {
    return "C";
  }
  if (key.find("resistance") != string::npos) {
    return "R";
  }
  if (key.find("inductance") != string::npos) {
    return "L";
  }
  if (key.find("power") != string::npos || key.find("watts") != string::npos) {
    return "Pwr";
  }
  if (key.find("reversevoltage") != string::npos || key == "vr") {
    return "Vr";
  }
  if (key.find("operatingvoltage") != string::npos || key.find("voltagerated") != string::npos ||
      key.find("ratedvoltage") != string::npos) {
    return "Vr";
  }
  if (key.find("voltagesupply") != string::npos || key == "voltage") {
    return "Vdd";
  }
  if (key.find("voltageoutput") != string::npos || key == "vout") {
    return "Vout";
  }
  if (key.find("voltageinput") != string::npos || key == "vin") {
    return "Vin";
  }
  if (key.find("forwardvoltage") != string::npos || key == "vf") {
    return "Vf";
  }
  if (key.find("reversestandoff") != string::npos) {
    return "Vst";
  }
  if (key.find("breakdown") != string::npos) {
    return "Vbr";
  }
  if (key.find("clamping") != string::npos) {
    return "Vc";
  }
  if (key.find("currentpeakpulse") != string::npos || key.find("peakpulsecurrent") != string::npos) {
    return "Ipp";
  }
  if (key.find("peakpulsepower") != string::npos) {
    return "Ppp";
  }
  if (key.find("saturationcurrent") != string::npos || key.find("isat") != string::npos) {
    return "Isat";
  }
  if (key.find("currentrating") != string::npos || key == "current") {
    return "I";
  }
  if (key.find("currentcontinuousdrain") != string::npos || key == "id") {
    return "Id";
  }
  if (key.find("collectoremittervoltage") != string::npos || key == "vce" || key == "vceo") {
    return "Vce";
  }
  if (key.find("collectorcurrent") != string::npos || key == "ic") {
    return "Ic";
  }
  if (key.find("drainsourcevoltage") != string::npos || key == "vdss" || key == "vds") {
    return "Vds";
  }
  if (key.find("rdson") != string::npos) {
    return "Rds";
  }
  if (key.find("gatecharge") != string::npos || key == "qg") {
    return "Qg";
  }
  if (key == "hfe" || key.find("dccurrentgain") != string::npos) {
    return "hFE";
  }
  if (key.find("frequency") != string::npos) {
    return "F";
  }
  if (key.find("loadcapacitance") != string::npos) {
    return "CL";
  }
  if (key.find("operatingmode") != string::npos) {
    return "Mode";
  }
  if (key.find("temperature") != string::npos) {
    return "Temp";
  }
  if (key.find("sensortype") != string::npos || key == "type") {
    return "Type";
  }
  if (key.find("outputtype") != string::npos || key == "output") {
    return "Out";
  }
  if (key.find("resolution") != string::npos) {
    return "Res";
  }
  if (key.find("accuracy") != string::npos) {
    return "Acc";
  }
  if (key.find("features") != string::npos) {
    return "Feat";
  }
  if (key.find("pins") != string::npos || key.find("numberofpositions") != string::npos || key.find("pincount") != string::npos) {
    return "Pins";
  }
  if (key.find("connector") != string::npos) {
    return "Conn";
  }
  if (key.find("rows") != string::npos) {
    return "Rows";
  }
  if (key.find("pitch") != string::npos) {
    return "Pitch";
  }
  if (key.find("shielding") != string::npos) {
    return "Shield";
  }
  if (key.find("composition") != string::npos) {
    return "Comp";
  }
  if (key.find("temperaturecoefficient") != string::npos || key.find("tempco") != string::npos) {
    return "Tempco";
  }
  if (key.find("coreprocessor") != string::npos || key == "core") {
    return "Core";
  }
  if (key.find("clockspeed") != string::npos || key.find("clockfrequency") != string::npos || key == "speed") {
    return "Clk";
  }
  if (key == "flash" || key.find("programmemorysize") != string::npos) {
    return "Flash";
  }
  if (key == "ram" || key == "memory") {
    return "RAM";
  }
  if (key.find("package") != string::npos) {
    return "Pkg";
  }

  return trim(label);
}

string shortValueLine(const string& label, optional<string> value, size_t maxLength) {
  if (!value) {
    return {};
  }

  const auto cleaned = trim(*value);
  if (cleaned.empty()) {
    return {};
  }

  const auto shortLabel = shortParameterLabel(label);
  if (shortLabel.empty()) {
    return fieldOrBlank(cleaned, maxLength);
  }

  return fieldOrBlank(shortLabel + " " + cleaned, maxLength);
}

string compactDescriptor(const string& value, size_t maxLength) {
  auto cleaned = trim(value);
  if (cleaned.empty()) {
    return {};
  }

  const auto cut = cleaned.find_first_of(",;(/");
  if (cut != string::npos) {
    cleaned = trim(cleaned.substr(0, cut));
  }

  return ellipsize(cleaned, maxLength);
}

string dateOnly(time_t value) {
  const auto ts = nowTimestampString(value);
  if (ts.size() >= 10) {
    return ts.substr(0, 10);
  }
  return ts;
}

string compactJoin(const vector<string>& parts, const string& separator) {
  vector<string> filtered;
  for (const auto& part : parts) {
    const auto cleaned = trim(part);
    if (!cleaned.empty()) {
      filtered.push_back(cleaned);
    }
  }
  return join(filtered, separator.empty() ? ' ' : separator.front());
}

string normalizeResistanceValue(string value) {
  value = trim(value);
  if (value.empty()) {
    return value;
  }

  auto lowered = toLower(value);
  auto stripSuffix = [&](const string& suffix) {
    if (lowered.size() < suffix.size()) {
      return false;
    }
    if (lowered.compare(lowered.size() - suffix.size(), suffix.size(), suffix) != 0) {
      return false;
    }
    value = trim(value.substr(0, value.size() - suffix.size()));
    lowered = toLower(value);
    return true;
  };

  stripSuffix(" ohms");
  stripSuffix(" ohm");
  stripSuffix("ohms");
  stripSuffix("ohm");

  value = trim(value);
  if (value.empty()) {
    return {};
  }

  return value + u8"\u03A9";
}

string fieldOrBlank(const string& value, size_t maxLength);

string fitSingleLineLabel(const string& value, size_t maxLength) {
  return fieldOrBlank(value, maxLength);
}

bool isCompactManufacturerPartNumber(const string& value) {
  const auto text = trim(value);
  if (text.empty() || text.size() > 18 || text.find_first_of(" \t") != string::npos) return false;
  bool hasLetter = false;
  bool hasDigit = false;
  for (const auto character : text) {
    const auto ch = static_cast<unsigned char>(character);
    if (isalpha(ch)) hasLetter = true;
    else if (isdigit(ch)) hasDigit = true;
    else if (character != '-' && character != '_' && character != '.' && character != '+') return false;
  }
  return hasLetter && hasDigit;
}

CableFlagFont cableFlagFont(const string& text) {
  const auto length = text.size();
  if (length <= 4) return {68, 58};
  if (length <= 6) return {56, 38};
  if (length <= 9) return {44, 26};
  if (length <= 13) return {36, 20};
  return {28, 16};
}

vector<string> wrapLabelLines(const string& value, size_t maxWidth, size_t maxLines) {
  vector<string> lines;
  if (maxWidth == 0 || maxLines == 0) {
    return lines;
  }

  istringstream input(trim(value));
  string word;
  string current;

  auto flushCurrent = [&]() {
    if (!current.empty()) {
      lines.push_back(current);
      current.clear();
    }
  };

  while (input >> word) {
    if (word.size() > maxWidth) {
      word = ellipsize(word, maxWidth);
    }

    if (current.empty()) {
      current = word;
      continue;
    }

    if (current.size() + 1 + word.size() <= maxWidth) {
      current.push_back(' ');
      current += word;
      continue;
    }

    flushCurrent();
    if (lines.size() >= maxLines) {
      break;
    }
    current = word;
  }

  flushCurrent();
  if (lines.size() > maxLines) {
    lines.resize(maxLines);
  }

  if (!input.eof() && !lines.empty()) {
    lines.back() = ellipsize(lines.back(), maxWidth);
  }

  return lines;
}

string sanitiseZplFragment(const string& value) {
  string output;
  output.reserve(value.size());
  bool previousSpace = false;
  for (char ch : value) {
    unsigned char uch = static_cast<unsigned char>(ch);
    if (ch == '^' || ch == '~') {
      if (!previousSpace && !output.empty()) {
        output.push_back(' ');
        previousSpace = true;
      }
      continue;
    }
    if (ch == '\r' || ch == '\n' || ch == '\t' || iscntrl(uch)) {
      if (!previousSpace && !output.empty()) {
        output.push_back(' ');
        previousSpace = true;
      }
      continue;
    }
    if (isspace(uch) != 0) {
      if (!previousSpace && !output.empty()) {
        output.push_back(' ');
        previousSpace = true;
      }
      continue;
    }
    output.push_back(ch);
    previousSpace = false;
  }
  return trim(output);
}

string fieldOrBlank(const string& value, size_t maxLength) {
  return sanitiseZplFragment(ellipsize(trim(value), maxLength));
}

string collectLineFromValues(initializer_list<string> values, const string& separator, size_t maxLength) {
  vector<string> parts;
  for (const auto& value : values) {
    const auto cleaned = trim(value);
    if (!cleaned.empty()) {
      parts.push_back(cleaned);
    }
  }
  if (parts.empty()) {
    return {};
  }
  return fieldOrBlank(join(parts, separator.empty() ? ' ' : separator.front()), maxLength);
}

}  // namespace label_printer_detail
}  // namespace inventatory
