// SPDX-FileCopyrightText: 2026 Paweł Kwiatkowski
// SPDX-License-Identifier: GPL-3.0-only

#include "core/inventory/ParameterLookup.h"

#include <algorithm>
#include <cctype>

namespace inventatory {

using namespace std;

string normalizeKey(string value) {
  string normalized;
  normalized.reserve(value.size());
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      normalized.push_back(static_cast<char>(tolower(ch)));
    }
  }
  return normalized;
}

// Lower-case words of a parameter name; punctuation separates them.
vector<string> parameterNameWords(const string& name) {
  vector<string> words;
  string word;
  for (const unsigned char ch : name) {
    if (isalnum(ch)) {
      word.push_back(static_cast<char>(tolower(ch)));
    } else if (!word.empty()) {
      words.push_back(word);
      word.clear();
    }
  }
  if (!word.empty()) words.push_back(word);
  return words;
}

// Names match when one's words lead the other's: "Package" matches "Package / Case",
// but "Color" does not match "Lens Color".
bool parameterLabelMatches(const string& lhs, const string& rhs) {
  const auto left = parameterNameWords(lhs);
  const auto right = parameterNameWords(rhs);
  if (left.empty() || right.empty()) return false;
  const auto& shorter = left.size() <= right.size() ? left : right;
  const auto& longer = left.size() <= right.size() ? right : left;
  return equal(shorter.begin(), shorter.end(), longer.begin());
}

bool looksLikePackagingValue(const string& value) {
  const auto normalized = normalizeKey(value);
  if (normalized.empty()) {
    return false;
  }

  static const initializer_list<const char*> kPackagingTokens = {
      "tapeandreel", "cuttape", "digireel", "tube", "tray", "bulk", "bag", "strip", "ammo", "box", "loose",
      "reel"};
  return any_of(kPackagingTokens.begin(), kPackagingTokens.end(), [&](const char* token) {
    return normalized == token || normalized.find(token) != string::npos;
  });
}

const Parameter* findParameter(const vector<Parameter>& parameters, initializer_list<const char*> names) {
  // An exact name match wins over a prefix match, even when the record lists the prefix first.
  for (const auto* name : names) {
    const auto wanted = normalizeKey(name);
    for (const auto& parameter : parameters) {
      if (normalizeKey(parameter.name) == wanted && !wanted.empty()) {
        return &parameter;
      }
    }
    for (const auto& parameter : parameters) {
      if (parameterLabelMatches(parameter.name, name)) {
        return &parameter;
      }
    }
  }
  return nullptr;
}

optional<string> parameterValue(const InventoryItem& item, initializer_list<const char*> names) {
  if (const auto* parameter = findParameter(item.parameters, names); parameter != nullptr) {
    const auto value = trim(parameter->value);
    if (!value.empty() && !looksLikePackagingValue(value)) {
      return value;
    }
  }
  return nullopt;
}

}  // namespace inventatory
