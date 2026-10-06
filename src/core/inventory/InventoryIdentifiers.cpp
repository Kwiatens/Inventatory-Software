// Inventatory - Inventory identifier and scan-code helpers.

#include "core/inventory/InventoryInternals.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace inventatory {

using namespace std;

string inventatoryCategoryPrefix(const string& category) {
  const auto lowered = toLower(trim(category));
  if (lowered.find("capacitor") != string::npos) {
    return "C";
  }
  if (lowered.find("resistor") != string::npos) {
    return "R";
  }
  if (lowered.find("inductor") != string::npos || lowered.find("choke") != string::npos ||
      lowered.find("coil") != string::npos) {
    return "L";
  }
  if (lowered.find("indicator") != string::npos || lowered.find("led") != string::npos) {
    return "I";
  }
  if (lowered.find("connector") != string::npos) {
    return "J";
  }
  if (lowered.find("microcontroller") != string::npos || lowered.find("mcu") != string::npos) {
    return "U";
  }
  if (lowered.find("integrated circuit") != string::npos || lowered == "ic" || lowered.find("ic ") != string::npos) {
    return "U";
  }
  if (lowered.find("sensor") != string::npos) {
    return "S";
  }
  if (lowered.find("switch") != string::npos) {
    return "K";
  }
  if (lowered.find("diode") != string::npos || lowered.find("rectifier") != string::npos) {
    return "D";
  }
  if (lowered.find("fuse") != string::npos) {
    return "F";
  }
  if (lowered.find("relay") != string::npos) {
    return "Y";
  }
  if (lowered.find("transistor") != string::npos || lowered.find("mosfet") != string::npos ||
      lowered.find("fet") != string::npos) {
    return "T";
  }

  for (char ch : lowered) {
    if (isalpha(static_cast<unsigned char>(ch))) {
      return string(1, static_cast<char>(toupper(static_cast<unsigned char>(ch))));
    }
  }

  return "X";
}

namespace {

bool parseInventatoryIdValue(const string& value, string& prefix, size_t& sequence) {
  const auto trimmed = trim(value);
  constexpr string_view kInventatoryIdPrefix = "Inventatory:";
  if (trimmed.rfind(kInventatoryIdPrefix, 0) != 0) {
    return false;
  }

  const auto prefixSize = kInventatoryIdPrefix.size();
  const auto dash = trimmed.find('-', prefixSize);
  if (dash == string::npos || dash <= prefixSize || dash + 1 >= trimmed.size()) {
    return false;
  }

  const auto rawPrefix = trimmed.substr(prefixSize, dash - prefixSize);
  if (rawPrefix.empty()) {
    return false;
  }

  size_t parsedSequence = 0;
  for (size_t index = dash + 1; index < trimmed.size(); ++index) {
    const char ch = trimmed[index];
    if (!isdigit(static_cast<unsigned char>(ch))) {
      return false;
    }
    parsedSequence = parsedSequence * 10 + static_cast<size_t>(ch - '0');
  }

  prefix = rawPrefix;
  sequence = parsedSequence;
  return true;
}

bool digitsOnly(const string& value) {
  return !value.empty() && all_of(value.begin(), value.end(), [](unsigned char ch) {
           return isdigit(ch) != 0;
         });
}

}  // namespace

string makeInventatoryId(const string& category, size_t sequence) {
  ostringstream out;
  out << "Inventatory:" << inventatoryCategoryPrefix(category) << '-' << uppercase << setw(5) << setfill('0') << sequence;
  return out.str();
}

bool isInventatoryId(const string& value) {
  string prefix;
  size_t sequence = 0;
  return parseInventatoryIdValue(value, prefix, sequence);
}

string formatMachineCode(size_t sequence) {
  ostringstream out;
  out << setw(4) << setfill('0') << sequence;
  return out.str();
}

string normalizeMachineCode(const string& value) {
  auto code = trim(value);
  if (!digitsOnly(code)) {
    return {};
  }

  if (code.size() < 4) {
    code.insert(code.begin(), 4 - code.size(), '0');
  }
  return code;
}

bool isMachineCode(const string& value) {
  return digitsOnly(trim(value));
}

string buildVisibleInventatoryId(const InventoryItem& item) {
  const auto code = normalizeMachineCode(item.machineCode);
  if (code.empty()) {
    return trim(item.inventatoryId);
  }

  string prefix = inventatoryCategoryPrefix(item.category);
  string parsedPrefix;
  size_t sequence = 0;
  if (parseInventatoryIdValue(item.inventatoryId, parsedPrefix, sequence) && !parsedPrefix.empty()) {
    prefix = parsedPrefix;
  }
  if (prefix.empty()) {
    return trim(item.inventatoryId);
  }

  return prefix + '-' + code;
}

bool matchesMachineCode(const string& machineCode, const string& code) {
  const auto needle = normalizeMachineCode(code);
  if (needle.empty()) {
    return false;
  }

  return normalizeMachineCode(machineCode) == needle;
}

void ensureInventoryIdentifiers(vector<InventoryItem>& items) {
  unordered_map<string, size_t> nextSequenceByPrefix;
  unordered_set<string> reservedMachineCodes;
  unordered_set<string> usedIds;
  unordered_set<string> keptIds;
  unordered_set<string> usedMachineCodes;
  size_t nextMachineSequence = 1;

  for (const auto& item : items) {
    if (!trim(item.inventatoryId).empty()) {
      usedIds.insert(toLower(trim(item.inventatoryId)));
      string prefix;
      size_t sequence = 0;
      if (parseInventatoryIdValue(item.inventatoryId, prefix, sequence)) {
        auto& nextSequence = nextSequenceByPrefix[prefix];
        nextSequence = max(nextSequence, sequence + 1);
      }
    }

    const auto normalizedMachineCode = normalizeMachineCode(item.machineCode);
    if (!normalizedMachineCode.empty()) {
      reservedMachineCodes.insert(normalizedMachineCode);
    }
  }

  for (auto& item : items) {
    if (item.createdAt == 0) {
      item.createdAt = item.lastUpdated == 0 ? nowEpoch() : item.lastUpdated;
    }

    auto normalizedId = trim(item.inventatoryId);
    // A well-formed ID is kept only by the first part that carries it; a later duplicate (copied or
    // imported data) gets a fresh one so the inventory stays saveable.
    if (!normalizedId.empty() && isInventatoryId(normalizedId) && keptIds.insert(toLower(normalizedId)).second) {
      item.inventatoryId = normalizedId;
    } else {
      const auto prefix = inventatoryCategoryPrefix(item.category);
      auto& nextSequence = nextSequenceByPrefix[prefix];
      if (nextSequence == 0) {
        nextSequence = 1;
      }

      string candidate;
      do {
        candidate = makeInventatoryId(item.category, nextSequence++);
      } while (usedIds.count(toLower(candidate)) != 0);

      item.inventatoryId = move(candidate);
      usedIds.insert(toLower(item.inventatoryId));
      keptIds.insert(toLower(item.inventatoryId));
    }

    auto normalizedMachineCode = normalizeMachineCode(item.machineCode);
    // Like Inventatory IDs, a machine code stays with the first part that carries it, so the printed
    // label of that part keeps resolving; only the later duplicate is given a new code.
    if (!normalizedMachineCode.empty() && usedMachineCodes.count(normalizedMachineCode) == 0) {
      item.machineCode = move(normalizedMachineCode);
      usedMachineCodes.insert(item.machineCode);
      continue;
    }

    do {
      normalizedMachineCode = formatMachineCode(nextMachineSequence++);
    } while (reservedMachineCodes.count(normalizedMachineCode) != 0 ||
             usedMachineCodes.count(normalizedMachineCode) != 0);

    item.machineCode = move(normalizedMachineCode);
    usedMachineCodes.insert(item.machineCode);
  }
}

bool validateInventoryIdentifiers(const vector<InventoryItem>& items, const vector<InventatoryRack>& racks) {
  unordered_set<string> itemIds;
  unordered_set<string> inventatoryIds;
  unordered_set<string> machineCodes;
  for (const auto& item : items) {
    const auto id = toLower(trim(item.id));
    if (id.empty() || !itemIds.insert(id).second) return false;

    const auto inventatoryId = toLower(trim(item.inventatoryId));
    if (!inventatoryId.empty() && !inventatoryIds.insert(inventatoryId).second) return false;

    const auto machineCode = normalizeMachineCode(item.machineCode);
    if (!machineCode.empty() && !machineCodes.insert(machineCode).second) return false;
  }

  unordered_set<string> rackIds;
  unordered_set<string> rackCodes;
  for (const auto& rack : racks) {
    const auto id = toLower(trim(rack.id));
    const auto code = toLower(trim(rack.code));
    if (id.empty() || code.empty() || !rackIds.insert(id).second || !rackCodes.insert(code).second) return false;
    if (rack.rows <= 0 || rack.columns <= 0 || rack.rows > 10000 || rack.columns > 10000) return false;
  }
  return true;
}

}  // namespace inventatory
