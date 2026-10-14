// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Recognises the main code of a supported vendor bag, and the vendor an inventory item came from.

#include "core/inventory/VendorBagCode.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace inventatory {

using namespace std;

namespace {

constexpr size_t kMaximumIdentifierBytes = 64;

// The ISO/IEC 15434 Macro 06 envelope: "[)>", a record separator, "06" and a group separator. The separators are split
// into their own literals so the hexadecimal escapes do not run into the digits that follow.
constexpr char kDigiKeyEnvelope[] = "[)>\x1e"
                                    "06\x1d";

bool isIso15434Separator(char character) {
  return character == '\x1d' || character == '\x1e' || character == '\x04';
}

bool isDigiKeyNumberCharacter(char character) {
  return isalnum(static_cast<unsigned char>(character)) || character == '-' || character == '_' || character == '.' ||
         character == '/' || character == '+';
}

// A DigiKey part number is written "445-15749-1-ND": letters, digits and separators, always ending in "-ND".
bool isDigiKeyPartNumber(const string& value) {
  if (value.size() <= 3 || value.size() > kMaximumIdentifierBytes) return false;
  if (value.compare(value.size() - 3, 3, "-ND") != 0) return false;
  return all_of(value.begin(), value.end(), isDigiKeyNumberCharacter);
}

// A manufacturer part number as a bag prints it. Accepts the characters manufacturers use in their numbers.
bool isManufacturerPartNumber(const string& value) {
  if (value.size() < 2 || value.size() > kMaximumIdentifierBytes) return false;
  return all_of(value.begin(), value.end(), [](char character) {
    return isalnum(static_cast<unsigned char>(character)) || strchr("-._/+#&,() ", character) != nullptr;
  });
}

// DigiKey bags carry their part number in the P identifier and the manufacturer part number in 1P. The P identifier
// must be read as a whole field: "30P" and "1P" are other identifiers.
optional<VendorBagIdentity> recognizeDigiKeyBag(const string& code) {
  const size_t envelopeSize = sizeof(kDigiKeyEnvelope) - 1;
  if (code.size() <= envelopeSize || code.compare(0, envelopeSize, kDigiKeyEnvelope) != 0) return nullopt;

  string partNumber;
  string manufacturerPartNumber;
  size_t index = envelopeSize;
  while (index < code.size()) {
    if (isIso15434Separator(code[index])) {
      ++index;
      continue;
    }
    const auto end = static_cast<size_t>(find_if(code.begin() + static_cast<long>(index), code.end(),
                                                 isIso15434Separator) - code.begin());
    const auto field = trim(code.substr(index, end - index));
    index = end;
    if (field.rfind("1P", 0) == 0) {
      if (manufacturerPartNumber.empty()) manufacturerPartNumber = trim(field.substr(2));
    } else if (field.rfind("P", 0) == 0) {
      if (partNumber.empty()) partNumber = trim(field.substr(1));
    }
  }

  if (!isDigiKeyPartNumber(partNumber)) return nullopt;
  if (!manufacturerPartNumber.empty() && !isManufacturerPartNumber(manufacturerPartNumber)) return nullopt;
  return VendorBagIdentity{"digikey", partNumber, manufacturerPartNumber, {}};
}

optional<VendorBagIdentity> recognizeTmeBag(const string& code) {
  const auto label = parseTmeLabelPayload(code);
  if (!label) return nullopt;
  return VendorBagIdentity{"tme", label->symbol, label->manufacturerPartNumber, label->productUrl};
}

}  // namespace

optional<VendorBagIdentity> recognizeVendorBagCode(const string& code) {
  if (code.empty() || code.size() > kMaximumVendorBagCodeBytes) return nullopt;
  if (auto digiKey = recognizeDigiKeyBag(code)) return digiKey;
  return recognizeTmeBag(code);
}

string originProvider(const InventoryItem& item) {
  const auto provider = toLower(trim(item.vendorMetadata.provider));
  if (!provider.empty()) return provider;
  if (!trim(item.digikeyPartNumber).empty()) return "digikey";
  return {};
}

}  // namespace inventatory
