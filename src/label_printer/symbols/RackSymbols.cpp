// Inventatory - Rack type to electrical symbol lookup.

#include "label_printer/symbols/RackSymbols.h"

#include <cctype>
#include <cstring>

namespace inventatory {

using namespace std;

namespace {

string normalizedType(const string& value) {
  string normalized;
  for (const auto character : value) {
    const auto ch = static_cast<unsigned char>(character);
    if (isalnum(ch)) normalized.push_back(static_cast<char>(tolower(ch)));
  }
  return normalized;
}

const RackSymbolSet* findSet(const char* key) {
  for (size_t index = 0; index < kRackSymbolSetCount; ++index) {
    if (strcmp(kRackSymbolSets[index].key, key) == 0) return &kRackSymbolSets[index];
  }
  return nullptr;
}

}  // namespace

string rackSymbolKey(const string& componentType) {
  struct Alias {
    const char* type;
    const char* key;
  };
  // Matches the singular and plural spellings the rack allocator treats as one type.
  static const Alias aliases[] = {
      {"resistor", "resistor"},           {"resistors", "resistor"},
      {"capacitor", "capacitor"},         {"capacitors", "capacitor"},
      {"inductor", "inductor"},           {"inductors", "inductor"},
      {"diode", "diode"},                 {"diodes", "diode"},
      {"indicator", "led"},               {"indicators", "led"},
      {"led", "led"},                     {"leds", "led"},
      {"transistor", "transistor"},       {"transistors", "transistor"},
      {"integratedcircuit", "ic"},        {"integratedcircuits", "ic"},
      {"ic", "ic"},                       {"ics", "ic"},
      {"timing", "crystal"},              {"crystal", "crystal"},
      {"crystals", "crystal"},            {"oscillator", "crystal"},
      {"oscillators", "crystal"},         {"fuse", "fuse"},
      {"fuses", "fuse"},                  {"connector", "connector"},
      {"connectors", "connector"},
  };
  const auto normalized = normalizedType(componentType);
  for (const auto& alias : aliases) {
    if (normalized == alias.type) return alias.key;
  }
  return "grid";
}

const RackSymbolBitmap& rackSymbolFor(const string& componentType, SymbolStandard standard) {
  const auto* set = findSet(rackSymbolKey(componentType).c_str());
  if (set == nullptr) set = findSet("grid");
  return standard == SymbolStandard::Us ? *set->us : *set->eu;
}

const char* symbolStandardKey(SymbolStandard standard) {
  return standard == SymbolStandard::Us ? "us" : "eu";
}

bool parseSymbolStandard(const string& text, SymbolStandard& standard) {
  const auto normalized = normalizedType(text);
  if (normalized == "eu") {
    standard = SymbolStandard::Eu;
    return true;
  }
  if (normalized == "us") {
    standard = SymbolStandard::Us;
    return true;
  }
  return false;
}

const char* symbolStandardLabel(SymbolStandard standard) {
  return standard == SymbolStandard::Us ? "US (ANSI/IEEE 315)" : "EU (IEC 60617)";
}

}  // namespace inventatory
