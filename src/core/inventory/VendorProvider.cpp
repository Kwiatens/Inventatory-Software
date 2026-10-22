// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// The supported vendors: the key each one is stored under, and the name it is shown with.

#include "core/inventory/VendorProvider.h"

#include "core/inventory/Inventory.h"

namespace inventatory {

using namespace std;

string vendorDisplayName(const string& provider) {
  const auto key = toLower(trim(provider));
  if (key == kVendorProviderDigiKey) return "DigiKey";
  if (key == kVendorProviderTme) return "TME";
  if (key == kVendorProviderMouser) return "Mouser";
  return {};
}

}  // namespace inventatory
