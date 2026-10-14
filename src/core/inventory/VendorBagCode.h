// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// Recognises the main code of a supported vendor bag, and the vendor an inventory item came from.

#pragma once

#include "core/inventory/Inventory.h"

#include <cstddef>
#include <optional>
#include <string>

namespace inventatory {

// The longest main code a device may send. A DigiKey Data Matrix with its identifiers runs to about 150 characters, and a
// TME label's QR text to about 130.
inline constexpr std::size_t kMaximumVendorBagCodeBytes = 512;

// The part a vendor bag's main code names.
struct VendorBagIdentity {
  // The vendor's key as stored in vendorMetadata.provider: "digikey" or "tme".
  std::string provider;
  // The vendor's own part number: a DigiKey part number ("445-15749-1-ND") or a TME symbol.
  std::string providerProductNumber;
  // The manufacturer part number the bag prints, when it carries one.
  std::string manufacturerPartNumber;
  // The vendor's product page, when the code gives one.
  std::string productUrl;
};

// Recognises a main code from a supported vendor bag:
//   - a DigiKey bag's Data Matrix: an ISO/IEC 15434 Macro 06 envelope whose P field is a DigiKey part number ending in -ND;
//   - a TME bag's QR code: key:value text with PN and MPN or MFR, as parseTmeLabelPayload reads it.
// Anything else, including the Data Matrix of a TME bag and an Inventatory machine code, gives nullopt.
std::optional<VendorBagIdentity> recognizeVendorBagCode(const std::string& code);

// The vendor an item came from, as a lower-case key ("digikey" or "tme"), or empty when no vendor is recorded.
// Items stored before origins were recorded have no provider; a DigiKey part number on such an item names DigiKey.
std::string originProvider(const InventoryItem& item);

}  // namespace inventatory
