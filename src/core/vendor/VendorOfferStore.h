// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Saved vendor offers, in vendor-offers.db beside inventory.db. It is a cache of what the vendors
// said, not inventory data: it is not in backups, and a lost file is filled again by a refresh.

#pragma once

#include "core/vendor/VendorOffer.h"

#include <filesystem>
#include <string>
#include <vector>

namespace inventatory {

inline constexpr const char* kVendorOfferDatabaseName = "vendor-offers.db";
inline constexpr int kVendorOfferSchemaVersion = 1;

std::filesystem::path vendorOfferDatabasePath(const std::filesystem::path& dataDirectory);

// Replaces every offer saved for one lookup (a provider and the product number it was asked for)
// with these, in one transaction. An empty list removes them. On failure, including an offer without
// a currency or price breaks, the earlier offers stay.
bool saveVendorOffers(const std::filesystem::path& databasePath, const std::string& provider,
                      const std::string& lookupKey, const std::vector<VendorOffer>& offers,
                      std::string* error = nullptr);

// The offers saved for one lookup, in the order they were saved. A missing file is no offers, not an
// error. A row that does not read back as a whole offer (bad currency, no price breaks) is left out.
bool loadVendorOffers(const std::filesystem::path& databasePath, const std::string& provider,
                      const std::string& lookupKey, std::vector<VendorOffer>& offers,
                      std::string* error = nullptr);

}  // namespace inventatory
