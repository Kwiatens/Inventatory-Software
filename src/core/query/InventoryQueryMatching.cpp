// Inventatory - Inventory query token matching and evaluation.

#include "core/query/InventoryQueryPrivate.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <limits>
#include <system_error>

namespace inventatory::query_detail {

using namespace std;

optional<PhysicalValueComparison> bestPhysicalComparison(const optional<PhysicalValueComparison>& current,
                                                         const optional<PhysicalValueComparison>& candidate) {
  if (!candidate.has_value()) return current;
  if (!current.has_value() || candidate->relativeDifference < current->relativeDifference) return candidate;
  return current;
}

namespace {

struct TokenMatchResult {
  bool matched = false;
  optional<PhysicalValueComparison> physical;
};

TokenMatchResult tokenMatchesParameterList(const vector<Parameter>& parameters, const string& value) {
  const auto equalsPos = value.find('=');
  const auto needleKey = toLower(equalsPos == string::npos ? value : value.substr(0, equalsPos));
  const auto needleValue = equalsPos == string::npos ? string() : trim(value.substr(equalsPos + 1));
  const auto loweredNeedleValue = toLower(needleValue);
  TokenMatchResult result;

  for (const auto& parameter : parameters) {
    const auto key = toLower(parameter.name);
    const auto parameterValue = toLower(parameter.value);

    if (!needleKey.empty() && key.find(needleKey) == string::npos) {
      continue;
    }

    if (needleValue.empty()) {
      continue;
    }

    // Try physical value matching first when the value looks like a physical quantity
    auto parsedNeedle = parsePhysicalValue(needleValue);
    if (parsedNeedle.has_value() && parsedNeedle->type != PhysicalValueType::Unknown) {
      // If a key was specified (e.g. "param:Capacitance=0.1uF"), only match
      // parameters whose name maps to the same physical type.
      if (!needleKey.empty()) {
        auto paramType = parameterNameToType(parameter.name);
        if (paramType != PhysicalValueType::Unknown && paramType != parsedNeedle->type) {
          continue;
        }
      }
      const auto comparison = comparePhysicalValues(parameter.value, needleValue);
      if (comparison.has_value() && comparison->band != PhysicalValueMatchBand::None) {
        result.matched = true;
        result.physical = bestPhysicalComparison(result.physical, comparison);
      }
    }

    // Fall back to substring match
    if (parameterValue.find(loweredNeedleValue) != string::npos) {
      result.matched = true;
    }
  }

  return result;
}

TokenMatchResult tokenMatchesParameter(const InventoryItem& item, const string& value) {
  const auto primary = tokenMatchesParameterList(item.parameters, value);
  const auto vendor = tokenMatchesParameterList(item.vendorMetadata.parameters, value);
  return {primary.matched || vendor.matched, bestPhysicalComparison(primary.physical, vendor.physical)};
}

// Reads the component family an item declares through its category or the leading word of its name
// ("CAP CER ...", "Resistor", "Crystal"). Returns nullopt for items that declare nothing, and for
// families without a physical value type (switches, MOSFETs, connectors).
optional<PhysicalValueType> declaredValueType(const InventoryItem& item) {
  const auto classify = [](const string& text) -> optional<PhysicalValueType> {
    const auto has = [&](const char* needle) { return text.find(needle) != string::npos; };
    if (has("capacitor") || has("supercap")) return PhysicalValueType::Capacitance;
    if (has("resistor") || has("thermistor") || has("potentiometer") || has("trimmer")) {
      return PhysicalValueType::Resistance;
    }
    if (has("inductor") || has("choke") || has("ferrite")) return PhysicalValueType::Inductance;
    if (has("crystal") || has("oscillator") || has("resonator")) return PhysicalValueType::Frequency;
    return nullopt;
  };
  if (const auto fromCategory = classify(toLower(item.category)); fromCategory.has_value()) return fromCategory;

  string word;
  for (const char character : item.partName) {
    if (!isalpha(static_cast<unsigned char>(character))) {
      if (!word.empty()) break;
      continue;
    }
    word.push_back(static_cast<char>(tolower(static_cast<unsigned char>(character))));
  }
  if (word == "cap") return PhysicalValueType::Capacitance;
  if (word == "res") return PhysicalValueType::Resistance;
  if (word == "ind") return PhysicalValueType::Inductance;
  if (word == "xtal" || word == "osc") return PhysicalValueType::Frequency;
  return classify(word);
}

optional<PhysicalValueComparison> tokenMatchesParameterPhysicallyList(const vector<Parameter>& parameters,
                                                                      const string& token,
                                                                      bool declaredMatch,
                                                                      bool allowOutsideBands) {
  const auto parsed = parsePhysicalValue(token);
  if (!parsed.has_value() || parsed->type == PhysicalValueType::Unknown) {
    return nullopt;
  }

  optional<PhysicalValueComparison> best;
  for (const auto& parameter : parameters) {
    // A parameter counts when it is named for this value type. Parameters of unknown meaning
    // ("Input Capacitance (Ciss)", "Contact Rating") are trusted only on an item that declares
    // itself a component of this type.
    const auto paramType = parameterNameToType(parameter.name);
    if (paramType == PhysicalValueType::Unknown ? !declaredMatch : paramType != parsed->type) continue;
    const auto comparison = comparePhysicalValues(parameter.value, token);
    if (comparison.has_value() && (allowOutsideBands || comparison->band != PhysicalValueMatchBand::None)) {
      best = bestPhysicalComparison(best, comparison);
    }
  }
  return best;
}

// Reads the whole operand as a decimal integer. Trailing text ("5abc", "1e3") or blanks make the
// token meaningless rather than silently truncating it; a value beyond long long saturates.
optional<long long> parseQuantityOperand(const string& text) {
  if (text.empty()) return nullopt;
  long long value = 0;
  const auto* first = text.data();
  const auto* last = text.data() + text.size();
  const auto result = from_chars(first, last, value);
  if (result.ec == errc::result_out_of_range && result.ptr == last) {
    return text.front() == '-' ? numeric_limits<long long>::min() : numeric_limits<long long>::max();
  }
  if (result.ec != errc{} || result.ptr != last) return nullopt;
  return value;
}

bool tokenMatchesQuantity(const InventoryItem& item, const string& token) {
  struct Comparison {
    const char* prefix;
    bool (*matches)(long long quantity, long long operand);
  };
  static const Comparison kComparisons[] = {
      {"qty>=", [](long long quantity, long long operand) { return quantity >= operand; }},
      {"qty<=", [](long long quantity, long long operand) { return quantity <= operand; }},
      {"qty>", [](long long quantity, long long operand) { return quantity > operand; }},
      {"qty<", [](long long quantity, long long operand) { return quantity < operand; }},
      {"qty=", [](long long quantity, long long operand) { return quantity == operand; }},
  };
  for (const auto& comparison : kComparisons) {
    const string prefix = comparison.prefix;
    if (token.rfind(prefix, 0) != 0) continue;
    const auto operand = parseQuantityOperand(token.substr(prefix.size()));
    return operand.has_value() && comparison.matches(item.quantity, *operand);
  }
  return false;
}

bool tokenMatchesStatus(const InventoryItem& item, const string& value, int lowStockThreshold) {
  const auto lowerValue = toLower(value);
  if (lowerValue == "low") {
    return isLowStock(item, lowStockThreshold);
  }
  if (lowerValue == "missing") {
    return item.hasMissingMetadata();
  }
  if (lowerValue == "synced") {
    return toLower(item.syncStatus) == "synced";
  }
  if (lowerValue == "unsynced") {
    return toLower(item.syncStatus) != "synced";
  }
  return containsInsensitive(item.syncStatus, lowerValue);
}

bool tokenMatchesCategory(const InventoryItem& item, const string& value) {
  return containsInsensitive(item.category, value);
}

bool tokenMatchesField(const string& field, const string& value) {
  return containsInsensitive(field, value);
}

}  // namespace

optional<PhysicalValueComparison> partNamePhysicalComparison(const InventoryItem& item, const string& target) {
  const auto parsedTarget = parsePhysicalValue(target);
  if (!parsedTarget.has_value() || parsedTarget->type == PhysicalValueType::Unknown) return nullopt;

  vector<string> words;
  string current;
  for (const char character : item.partName) {
    if (isspace(static_cast<unsigned char>(character)) || character == ',' || character == ';' ||
        character == '/' || character == '(' || character == ')') {
      if (!current.empty()) words.push_back(current);
      current.clear();
    } else {
      current.push_back(character);
    }
  }
  if (!current.empty()) words.push_back(current);

  optional<PhysicalValueComparison> best;
  const auto consider = [&](const string& candidate) {
    const auto comparison = comparePhysicalValues(candidate, target);
    if (comparison.has_value()) best = bestPhysicalComparison(best, comparison);
  };
  for (size_t index = 0; index < words.size(); ++index) {
    consider(words[index]);
    // Values written with a space, such as "0.1 uF" or "10 k".
    if (index + 1 < words.size()) consider(words[index] + " " + words[index + 1]);
  }
  return best;
}

optional<PhysicalValueComparison> itemPhysicalComparison(const InventoryItem& item, const string& target,
                                                         bool allowOutsideBands) {
  const auto parsed = parsePhysicalValue(target);
  if (!parsed.has_value() || parsed->type == PhysicalValueType::Unknown) return nullopt;

  // An item that says it is a different kind of component never matches, whatever numbers its
  // parameters or name happen to contain.
  const auto declared = declaredValueType(item);
  if (declared.has_value() && *declared != parsed->type) return nullopt;
  const bool declaredMatch = declared.has_value();

  auto best = bestPhysicalComparison(
      tokenMatchesParameterPhysicallyList(item.parameters, target, declaredMatch, allowOutsideBands),
      tokenMatchesParameterPhysicallyList(item.vendorMetadata.parameters, target, declaredMatch,
                                          allowOutsideBands));
  const auto fromName = partNamePhysicalComparison(item, target);
  if (fromName.has_value() && (allowOutsideBands || fromName->band != PhysicalValueMatchBand::None)) {
    best = bestPhysicalComparison(best, fromName);
  }
  return best;
}

QueryMatchResult evaluateQueryWithRack(const InventoryItem& item, const string& query, const string& itemRackLocation,
                                       int lowStockThreshold) {
  const auto tokens = tokenizeQuery(query);
  if (tokens.empty()) return {true, nullopt};

  optional<PhysicalValueComparison> bestPhysical;
  for (const auto& rawToken : tokens) {
    const auto token = toLower(rawToken);
    if (tokenMatchesQuantity(item, token)) continue;

    if (token.rfind("cat:", 0) == 0 || token.rfind("category:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesCategory(item, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("mfg:", 0) == 0 || token.rfind("manufacturer:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.manufacturer, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("name:", 0) == 0 || token.rfind("part:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.partName, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("loc:", 0) == 0 || token.rfind("location:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.location, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("rack:", 0) == 0) {
      const auto value = token.substr(5);
      if (tokenMatchesField(itemRackLocation, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("sku:", 0) == 0) {
      const auto value = token.substr(4);
      if (tokenMatchesField(item.sku, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("inventatory:", 0) == 0 || token.rfind("inventatoryid:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.inventatoryId, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("dg:", 0) == 0 || token.rfind("digikey:", 0) == 0) {
      const auto value = token.substr(token.find(':') + 1);
      if (tokenMatchesField(item.digikeyPartNumber, value)) continue;
      return {false, nullopt};
    }
    if (token.rfind("tag:", 0) == 0) {
      const auto value = token.substr(4);
      const auto matched = any_of(item.tags.begin(), item.tags.end(), [&](const string& tag) {
        return containsInsensitive(tag, value);
      });
      if (matched) continue;
      return {false, nullopt};
    }
    if (token.rfind("param:", 0) == 0) {
      const auto parameterMatch = tokenMatchesParameter(item, rawToken.substr(6));
      if (parameterMatch.matched) {
        bestPhysical = bestPhysicalComparison(bestPhysical, parameterMatch.physical);
        continue;
      }
      return {false, nullopt};
    }
    if (token.rfind("status:", 0) == 0) {
      if (tokenMatchesStatus(item, token.substr(7), lowStockThreshold)) continue;
      return {false, nullopt};
    }

    // Prefer physical comparison for parseable values so normalized matches
    // receive a rank even when the raw spelling also appears in the item text.
    const auto physicalMatch = itemPhysicalComparison(item, rawToken);
    if (physicalMatch.has_value()) {
      bestPhysical = bestPhysicalComparison(bestPhysical, physicalMatch);
      continue;
    }
    if (containsInsensitive(item.searchableText(), token)) continue;
    if (containsInsensitive(itemRackLocation, token)) continue;
    return {false, nullopt};
  }

  return {true, bestPhysical};
}

}  // namespace inventatory::query_detail
