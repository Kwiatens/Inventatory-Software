// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Recognises the main code of a supported vendor bag, and the vendor an inventory item came from.

#pragma once

#include "core/inventory/Inventory.h"

#include <cstddef>
#include <optional>
#include <string>

namespace inventatory {

// Longest main code a device may send: about 150 characters for DigiKey, 130 for TME labels.
inline constexpr std::size_t kMaximumVendorBagCodeBytes = 512;

// The part a vendor bag's main code names.
struct VendorBagIdentity {
  // The vendor's key as stored in vendorMetadata.provider: "digikey", "tme" or "mouser".
  std::string provider;
  // The vendor's own part number: a DigiKey part number ("445-15749-1-ND") or a TME symbol. Empty for Mouser: its
  // bag names no Mouser number, so the lookup runs on the manufacturer part number.
  std::string providerProductNumber;
  // The manufacturer part number the bag prints, when it carries one.
  std::string manufacturerPartNumber;
  // The vendor's product page, when the code gives one.
  std::string productUrl;
  // The manufacturer the bag prints, when it carries one. Only a Mouser bag reads it, to tell products apart.
  std::string manufacturer;
};

// Recognises a vendor bag's main code: a DigiKey Data Matrix (ISO/IEC 15434 Macro 06, P field
// ending in -ND), a Mouser Data Matrix (Macro 06 with the 14K line and 11K invoice fields, P holding the
// customer part number, which Mouser fills with the manufacturer part number), or a TME QR label as
// parseTmeLabelPayload reads it. Other codes give nullopt.
std::optional<VendorBagIdentity> recognizeVendorBagCode(const std::string& code);

// Lower-case vendor key ("digikey", "tme" or "mouser") an item came from; empty if unrecorded.
// Older items with no provider but a DigiKey part number count as DigiKey.
std::string originProvider(const InventoryItem& item);

}  // namespace inventatory
