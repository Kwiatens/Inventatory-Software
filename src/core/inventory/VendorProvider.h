// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// The supported vendors: the key each one is stored under, and the name it is shown with.

#pragma once

#include <string>

namespace inventatory {

// The vendor keys as stored in vendorMetadata.provider and on a vendor bag's identity.
inline constexpr const char* kVendorProviderDigiKey = "digikey";
inline constexpr const char* kVendorProviderTme = "tme";
inline constexpr const char* kVendorProviderMouser = "mouser";

// Display name ("DigiKey", "TME" or "Mouser") for a key, case-insensitive. Empty if unknown.
std::string vendorDisplayName(const std::string& provider);

}  // namespace inventatory
