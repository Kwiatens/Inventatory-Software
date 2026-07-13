// HIMS - Hardware Inventory Management System
// Internal helpers shared by inventory implementation files.

#pragma once

#include "core/Inventory.h"

namespace hims {

time_t nowEpoch();
string sanitizeIdPart(const string& value);
string compactHimsDisplayCode(const string& himsId);
string compactHimsBarcodeCode(const string& himsId);
bool matchesHimsScanCode(const string& himsId, const string& code);
string formatMachineCode(size_t sequence);
string normalizeMachineCode(const string& value);
bool isMachineCode(const string& value);
string buildVisibleHimsId(const InventoryItem& item);
bool matchesMachineCode(const string& machineCode, const string& code);

}  // namespace hims
