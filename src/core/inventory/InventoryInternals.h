// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

// Internal helpers shared by inventory implementation files.

#pragma once

#include "core/inventory/Inventory.h"

namespace inventatory {

time_t nowEpoch();
string sanitizeIdPart(const string& value);
string formatMachineCode(size_t sequence);
string normalizeMachineCode(const string& value);
bool isMachineCode(const string& value);
string buildVisibleInventatoryId(const InventoryItem& item);
bool matchesMachineCode(const string& machineCode, const string& code);

}  // namespace inventatory
