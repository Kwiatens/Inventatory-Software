// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Inventatory - Hardware Inventory Management System
// The supported vendors: the key each one is stored under, and the name it is shown with.

#pragma once

#include <string>

namespace inventatory {

// The vendor keys as stored in vendorMetadata.provider and on a vendor bag's identity.
inline constexpr const char* kVendorProviderDigiKey = "digikey";
inline constexpr const char* kVendorProviderTme = "tme";
inline constexpr const char* kVendorProviderMouser = "mouser";

// The name a vendor is shown with ("DigiKey", "TME" or "Mouser") for its key, which is compared without regard to case
// or surrounding spaces. Empty for an empty or unknown key.
std::string vendorDisplayName(const std::string& provider);

}  // namespace inventatory
