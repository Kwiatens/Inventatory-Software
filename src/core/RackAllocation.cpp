// HIMS - Hardware Inventory Management System
// Rack classification, allocation, and manual placement validation.

#include "core/Inventory.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace hims {

using namespace std;

namespace {

bool containsAny(const string& text, initializer_list<const char*> values) {
  return any_of(values.begin(), values.end(), [&](const char* value) { return containsInsensitive(text, value); });
}

string normalizedRackType(string value) {
  value = toLower(trim(value));
  value.erase(remove_if(value.begin(), value.end(), [](unsigned char ch) { return !isalnum(ch); }), value.end());
  return value;
}

bool sameRackType(const string& lhs, const string& rhs) {
  return normalizedRackType(lhs) == normalizedRackType(rhs);
}

string classificationText(const InventoryItem& item) {
  string text = item.category + " " + item.partName + " " + item.notes;
  for (const auto& tag : item.tags) text += " " + tag;
  for (const auto& parameter : item.parameters) text += " " + parameter.name + " " + parameter.value;
  return toLower(text);
}

bool isBulky(const string& text) {
  return containsAny(text, {"module", "development board", "dev board", "dev kit", "breakout", "evaluation board",
                            "esp32", "esp8266", "to-220", "to220", "to-247", "to247", "to-3 ", "to-3,",
                            "to-126", "to126", "to-218", "to218", "to-262", "to262", "to-263", "to263",
                            "d2pak", "d-pak", "dpak", "power module"});
}

bool hasThroughHoleEvidence(const string& text) {
  return containsAny(text, {"through hole", "through-hole", "tht", "radial", "axial", "wire leads", "wire lead",
                            "pc pins", "pc pin", "to-92", "to92", "dip-", "pdip", "cdip"});
}

bool hasSurfaceMountEvidence(const string& text) {
  return containsAny(text, {"surface mount", "surface-mount", "smd", "smt"});
}

bool hasChipPassivePackage(const string& text) {
  return containsAny(text, {"01005", "0201", "0402", "0603", "0805", "1206", "1210", "1806", "1812", "2010",
                            "2512"});
}

bool hasCompactSemiconductorPackage(const string& text) {
  return containsAny(text, {"sot-23", "sot23", "sot-323", "sot323", "sot-363", "sot363", "sc-70", "sc70",
                            "soic", "sop", "ssop", "tssop", "msop", "qfn", "dfn", "qfp", "tqfp", "lqfp",
                            "bga", "wson", "vson", "ucsp", "csp"});
}

bool hasCompactDiscretePackage(const string& text) {
  return containsAny(text, {"sod-123", "sod123", "sod-323", "sod323", "sod-523", "sod523", "sma", "smb", "smc",
                            "mini-melf", "minimelf"});
}

bool isSmdRackEligible(const string& text) {
  return !isBulky(text) && !hasThroughHoleEvidence(text) &&
         (hasSurfaceMountEvidence(text) || hasChipPassivePackage(text) || hasCompactSemiconductorPackage(text) ||
          hasCompactDiscretePackage(text));
}

optional<string> componentTypeFor(const InventoryItem& item) {
  const auto text = classificationText(item);
  if (containsAny(text, {"fuse", "fuses"})) return "Fuses";
  if (containsAny(text, {"connector", "connectors"})) return "Connectors";
  if (containsAny(text, {"transistor", "transistors", "mosfet", " fet", "bjt", "npn", "pnp"})) {
    return "Transistors";
  }
  if (containsAny(text, {"integrated circuit", "integrated circuits", "microcontroller", " mcu", "logic", "memory",
                         "op amp", "op-amp", "amplifier", "regulator", "power management", "converter ic",
                         "switching regulator", "buck", "boost", "step-down", "step-up", "pmic"})) {
    return "Integrated Circuits";
  }
  if (!isSmdRackEligible(text)) return nullopt;
  if (containsAny(text, {"resistor", "resistors"})) return "Resistors";
  if (containsAny(text, {"capacitor", "capacitors"})) return "Capacitors";
  if (containsAny(text, {"inductor", "inductors", "choke", "coil"})) return "Inductors";
  if (containsAny(text, {"diode", "diodes", "rectifier", "schottky", "zener", "tvs"})) return "Diodes";
  if (containsAny(text, {"indicator", "indicators", " led", "led ", "light emitting"})) return "Indicators";
  if (containsAny(text, {"crystal", "oscillator", "resonator"})) {
    return "Timing";
  }
  return nullopt;
}

int rackNumber(const string& code) {
  if (code.size() < 2 || toupper(static_cast<unsigned char>(code.front())) != 'R') return 0;
  try { return stoi(code.substr(1)); } catch (...) { return 0; }
}

bool slotOccupied(const InventoryStore& store, const string& rackId, const string& slot, const InventoryItem* except) {
  return any_of(store.items().begin(), store.items().end(), [&](const InventoryItem& candidate) {
    return &candidate != except && (except == nullptr || candidate.id != except->id) &&
           candidate.rackId == rackId && candidate.rackSlot == slot;
  });
}

string firstFreeSlot(const InventoryStore& store, const HimsRack& rack, const InventoryItem* except) {
  for (int row = 0; row < rack.rows; ++row) {
    for (int column = 1; column <= rack.columns; ++column) {
      const string slot = string(1, static_cast<char>('A' + row)) + to_string(column);
      if (!slotOccupied(store, rack.id, slot, except)) return slot;
    }
  }
  return {};
}

HimsRack* findRack(InventoryStore& store, const string& id) {
  const auto it = find_if(store.racks().begin(), store.racks().end(), [&](const HimsRack& rack) { return rack.id == id; });
  return it == store.racks().end() ? nullptr : &*it;
}

}  // namespace

string rackAssignmentModeName(RackAssignmentMode mode) {
  if (mode == RackAssignmentMode::Manual) return "manual";
  if (mode == RackAssignmentMode::Unassigned) return "unassigned";
  return "automatic";
}

RackAssignmentMode parseRackAssignmentMode(const string& value) {
  const auto lowered = toLower(trim(value));
  if (lowered == "manual") return RackAssignmentMode::Manual;
  if (lowered == "unassigned") return RackAssignmentMode::Unassigned;
  return RackAssignmentMode::Automatic;
}

string rackLocation(const InventoryItem& item, const vector<HimsRack>& racks) {
  if (item.rackId.empty() || item.rackSlot.empty()) return {};
  const auto it = find_if(racks.begin(), racks.end(), [&](const HimsRack& rack) { return rack.id == item.rackId; });
  return it == racks.end() ? string() : it->code + "-" + item.rackSlot;
}

bool isValidRackSlot(const string& value) {
  const auto slot = toLower(trim(value));
  return slot.size() == 2 && slot[0] >= 'a' && slot[0] <= 'e' && slot[1] >= '1' && slot[1] <= '5';
}

bool reconcileRackAssignment(InventoryStore& store, InventoryItem& item) {
  if (item.rackAssignment != RackAssignmentMode::Automatic) return false;
  const auto componentType = componentTypeFor(item);
  auto* currentRack = findRack(store, item.rackId);
  if (!componentType) {
    const bool changed = item.rackAssignment != RackAssignmentMode::Unassigned || !item.rackId.empty() ||
                         !item.rackSlot.empty();
    item.rackId.clear();
    item.rackSlot.clear();
    item.rackAssignment = RackAssignmentMode::Unassigned;
    return changed;
  }
  if (currentRack != nullptr && sameRackType(currentRack->componentType, *componentType) && isValidRackSlot(item.rackSlot) &&
      !slotOccupied(store, item.rackId, item.rackSlot, &item)) return false;

  item.rackId.clear();
  item.rackSlot.clear();
  vector<HimsRack*> compatible;
  for (auto& rack : store.racks())
    if (sameRackType(rack.componentType, *componentType)) compatible.push_back(&rack);
  sort(compatible.begin(), compatible.end(), [](const HimsRack* lhs, const HimsRack* rhs) {
    return rackNumber(lhs->code) < rackNumber(rhs->code);
  });
  for (auto* rack : compatible) {
    const auto slot = firstFreeSlot(store, *rack, &item);
    if (!slot.empty()) {
      item.rackId = rack->id;
      item.rackSlot = slot;
      item.rackAssignment = RackAssignmentMode::Manual;
      return true;
    }
  }

  int nextNumber = 1;
  for (const auto& rack : store.racks()) nextNumber = max(nextNumber, rackNumber(rack.code) + 1);
  HimsRack rack;
  rack.id = makeId();
  rack.code = "R" + to_string(nextNumber);
  rack.componentType = *componentType;
  rack.createdAt = time(nullptr);
  store.racks().push_back(rack);
  item.rackId = rack.id;
  item.rackSlot = "A1";
  item.rackAssignment = RackAssignmentMode::Manual;
  return true;
}

bool reconcileRackAssignments(InventoryStore& store) {
  bool changed = false;
  for (auto& item : store.items()) changed = reconcileRackAssignment(store, item) || changed;
  return changed;
}

bool setManualRackLocation(InventoryStore& store, InventoryItem& item, const string& value, string& error) {
  const auto requested = trim(value);
  error.clear();
  if (requested.empty()) {
    item.rackId.clear();
    item.rackSlot.clear();
    item.rackAssignment = RackAssignmentMode::Unassigned;
    return true;
  }
  if (toLower(requested) == "auto") {
    item.rackId.clear();
    item.rackSlot.clear();
    item.rackAssignment = RackAssignmentMode::Automatic;
    return true;
  }
  const auto dash = requested.find('-');
  if (dash == string::npos) {
    error = "Use R3-E3, AUTO, or leave blank";
    return false;
  }
  const auto code = toLower(trim(requested.substr(0, dash)));
  auto slot = toLower(trim(requested.substr(dash + 1)));
  transform(slot.begin(), slot.end(), slot.begin(), [](unsigned char ch) { return static_cast<char>(toupper(ch)); });
  if (!isValidRackSlot(slot)) {
    error = "Rack slot must be A1 through E5";
    return false;
  }
  const auto rack = find_if(store.racks().begin(), store.racks().end(), [&](const HimsRack& candidate) {
    return toLower(candidate.code) == code;
  });
  if (rack == store.racks().end()) {
    error = "Rack " + requested.substr(0, dash) + " does not exist";
    return false;
  }
  if (slotOccupied(store, rack->id, slot, &item)) {
    error = rack->code + "-" + slot + " is already occupied";
    return false;
  }
  item.rackId = rack->id;
  item.rackSlot = slot;
  item.rackAssignment = RackAssignmentMode::Manual;
  return true;
}

int rackNumberFromCode(const string& code) {
  return rackNumber(code);
}

string rackSlotLabel(int row, int column) {
  if (row < 0 || row >= 5 || column < 0 || column >= 5) return {};
  return string(1, static_cast<char>('A' + row)) + to_string(column + 1);
}

size_t rackOccupiedSlotCount(const InventoryStore& store, const HimsRack& rack) {
  return static_cast<size_t>(count_if(store.items().begin(), store.items().end(), [&](const InventoryItem& item) {
    return item.rackId == rack.id && isValidRackSlot(item.rackSlot);
  }));
}

InventoryItem* itemAtRackSlot(InventoryStore& store, const string& rackId, const string& slot) {
  const auto normalizedSlot = trim(slot);
  const auto it = find_if(store.items().begin(), store.items().end(), [&](const InventoryItem& item) {
    return item.rackId == rackId && item.rackSlot == normalizedSlot;
  });
  return it == store.items().end() ? nullptr : &*it;
}

const InventoryItem* itemAtRackSlot(const InventoryStore& store, const string& rackId, const string& slot) {
  const auto normalizedSlot = trim(slot);
  const auto it = find_if(store.items().begin(), store.items().end(), [&](const InventoryItem& item) {
    return item.rackId == rackId && item.rackSlot == normalizedSlot;
  });
  return it == store.items().end() ? nullptr : &*it;
}

bool moveItemToRackSlot(InventoryStore& store, InventoryItem& item, const HimsRack& rack, const string& slot,
                        string& error) {
  error.clear();
  if (!isValidRackSlot(slot)) {
    error = "Rack slot must be A1 through E5";
    return false;
  }
  if (slotOccupied(store, rack.id, slot, &item)) {
    error = rack.code + "-" + slot + " is already occupied";
    return false;
  }
  item.rackId = rack.id;
  item.rackSlot = slot;
  item.rackAssignment = RackAssignmentMode::Manual;
  return true;
}

bool unassignItemFromRack(InventoryItem& item) {
  const bool changed = item.rackAssignment != RackAssignmentMode::Unassigned || !item.rackId.empty() ||
                       !item.rackSlot.empty();
  item.rackId.clear();
  item.rackSlot.clear();
  item.rackAssignment = RackAssignmentMode::Unassigned;
  return changed;
}

bool restoreAutomaticRackAssignment(InventoryStore& store, InventoryItem& item) {
  item.rackId.clear();
  item.rackSlot.clear();
  item.rackAssignment = RackAssignmentMode::Automatic;
  return reconcileRackAssignment(store, item);
}

}  // namespace hims
