// Inventatory - Label text and electrical-value formatting helpers.

#include "label_printer/core/LabelPrinterPrivate.h"

#include "core/inventory/InventoryInternals.h"
#include "core/parts/PartDescriptor.h"
#include "ui/shared/AppUiShared.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>

namespace inventatory {

using namespace std;

namespace label_printer_detail {

string shortCode(const string& value, size_t maxLength) {
  return ellipsize(trim(value), maxLength);
}

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

string fitSingleLineLabel(const string& value, size_t maxLength) {
  return fieldOrBlank(value, maxLength);
}

namespace {

// Advance width of one byte of font 0 text, in hundredths of the ^A0 width
// parameter, checked against test prints: letters and digits came out at or
// below these values, while "-" (1.5 digits) and symbols print much wider than
// in a typical condensed face. A UTF-8 lead byte stands for its whole
// character; continuation bytes add nothing.
int font0ByteHundredths(unsigned char ch) {
  if ((ch & 0xC0) == 0x80) return 0;  // UTF-8 continuation byte
  if (ch >= 0x80) return 62;
  if (ch == ' ') return 24;
  if (ch == '-') return 78;  // measured: 1.5 digits wide
  if (strchr("Iil.,:;'!|", ch) != nullptr) return 28;
  if (strchr("MWmw", ch) != nullptr) return 78;
  if (isdigit(ch)) return 50;
  if (isupper(ch)) return 56;
  if (islower(ch)) return 47;
  return 66;
}

// 64-bit so that long text cannot overflow while it is being measured.
int64_t font0Hundredths(const string& text) {
  int64_t hundredths = 0;
  for (const auto character : text) hundredths += font0ByteHundredths(static_cast<unsigned char>(character));
  return hundredths;
}

int64_t font0Width(int64_t hundredths, int width) { return (hundredths * width + 99) / 100; }

int saturateToInt(int64_t value) {
  return static_cast<int>(clamp<int64_t>(value, numeric_limits<int>::min(), numeric_limits<int>::max()));
}

}  // namespace

int estimateFont0Width(const string& text, int height, int width) {
  (void)height;
  return saturateToInt(font0Width(font0Hundredths(text), width));
}

FittedLabelText fitFont0Text(const string& text, int maxWidth, initializer_list<int> sizes) {
  FittedLabelText fitted;
  const auto cleaned = sanitiseZplFragment(text);
  if (cleaned.empty() || sizes.size() == 0) return fitted;
  const auto cleanedHundredths = font0Hundredths(cleaned);
  for (const auto size : sizes) {
    const auto width = font0Width(cleanedHundredths, size);
    if (width <= maxWidth) return {cleaned, size, saturateToInt(width)};
  }
  // Nothing fits: keep the smallest size and drop trailing characters, one
  // character at a time from the end, until the text plus its ellipsis fits.
  // Only trailing bytes are removed, so the width of each candidate is kept as
  // a running total instead of being measured again: the search is linear in
  // the text length. The first candidate that fits is the same as before.
  const auto size = *(sizes.end() - 1);
  const auto ellipsisHundredths = font0Hundredths("...");
  size_t end = cleaned.size();
  int64_t hundredths = cleanedHundredths;  // of cleaned[0, end)
  while (end > 0) {
    do {
      hundredths -= font0ByteHundredths(static_cast<unsigned char>(cleaned[--end]));
    } while (end > 0 && (static_cast<unsigned char>(cleaned[end - 1]) & 0xC0) == 0x80);
    if (end > 0 && (static_cast<unsigned char>(cleaned[end - 1]) & 0x80) != 0) {
      hundredths -= font0ByteHundredths(static_cast<unsigned char>(cleaned[--end]));  // lead byte
    }
    if (end == 0) break;
    // The candidate is trim(cut) + "...", so trailing spaces of the cut are not measured.
    auto visible = end;
    auto visibleHundredths = hundredths;
    while (visible > 0 && isspace(static_cast<unsigned char>(cleaned[visible - 1])) != 0) {
      visibleHundredths -= font0ByteHundredths(static_cast<unsigned char>(cleaned[--visible]));
    }
    const auto width = font0Width(visibleHundredths + ellipsisHundredths, size);
    if (width <= maxWidth) return {trim(cleaned.substr(0, end)) + "...", size, saturateToInt(width)};
  }
  return {"...", size, estimateFont0Width("...", size, size)};
}

CableFlagFont cableFlagFont(const string& text) {
  const auto length = text.size();
  if (length <= 4) return {68, 58};
  if (length <= 6) return {56, 38};
  if (length <= 9) return {44, 26};
  if (length <= 13) return {36, 20};
  return {28, 16};
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

}  // namespace label_printer_detail
}  // namespace inventatory
