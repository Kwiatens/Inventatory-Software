// Inventatory - DigiKey product extraction and search matching.

#include "platform/DigiKeyApiPrivate.h"

#ifdef _WIN32

#include "core/PartDescriptor.h"
#include "platform/Environment.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <regex>
#include <sstream>
#include <utility>

namespace inventatory {
using namespace std;
namespace digikey_detail {

string normalizeParameterKey(const string& value) {
  string normalized;
  normalized.reserve(value.size());
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      normalized.push_back(static_cast<char>(tolower(ch)));
    }
  }
  return normalized;
}

bool looksLikeFrequencyValue(const string& value) {
  const auto normalized = normalizeParameterKey(value);
  return normalized.find("hz") != string::npos;
}

bool looksLikeInductanceValue(const string& value) {
  const auto normalized = normalizeParameterKey(value);
  if (normalized.empty() || normalized.find("hz") != string::npos) {
    return false;
  }

  if (normalized.find("uh") != string::npos || normalized.find("nh") != string::npos ||
      normalized.find("ph") != string::npos || normalized.find("henry") != string::npos) {
    return true;
  }

  return normalized.find_first_of("0123456789") != string::npos && !normalized.empty() && normalized.back() == 'h';
}

optional<string> readParameterText(const JsonPtr& entry, const string& label) {
  vector<string> candidates;
  for (const auto* key : {"ParameterValue", "ValueText", "Value"}) {
    if (const auto* member = findMember(entry, key); member != nullptr) {
      const auto text = valueText(*member);
      if (!text.empty()) {
        candidates.push_back(text);
      }
    }
  }

  if (candidates.empty()) {
    return nullopt;
  }

  const auto normalizedLabel = normalizeParameterKey(label);
  const auto chooseFirstMatching = [&](auto predicate) -> optional<string> {
    for (const auto& candidate : candidates) {
      if (predicate(candidate)) {
        return candidate;
      }
    }
    return nullopt;
  };

  if (normalizedLabel.find("inductance") != string::npos || normalizedLabel == "l") {
    if (const auto inductance = chooseFirstMatching(looksLikeInductanceValue); inductance.has_value()) {
      return inductance;
    }
    if (const auto nonFrequency = chooseFirstMatching([&](const string& candidate) {
          return !looksLikeFrequencyValue(candidate);
        });
        nonFrequency.has_value()) {
      return nonFrequency;
    }
    return nullopt;
  }

  if (normalizedLabel.find("frequency") != string::npos || normalizedLabel == "f") {
    if (const auto frequency = chooseFirstMatching(looksLikeFrequencyValue); frequency.has_value()) {
      return frequency;
    }
  }

  return candidates.front();
}

bool looksLikePackagingType(const string& value) {
  string normalized;
  normalized.reserve(value.size());
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      normalized.push_back(static_cast<char>(tolower(ch)));
    }
  }
  if (normalized.empty()) {
    return false;
  }

  static const initializer_list<const char*> kPackagingTokens = {
      "tapeandreel", "cuttape", "digireel", "reel", "tube", "tray", "bulk", "bag", "strip", "ammo", "box",
      "loose", "pack"};
  return any_of(kPackagingTokens.begin(), kPackagingTokens.end(), [&](const char* token) {
    return normalized == token || normalized.find(token) != string::npos;
  });
}

optional<string> readParameterValue(const JsonPtr& product, initializer_list<const char*> names) {
  const auto* entries = asArray(findMember(product, "Parameters") == nullptr ? nullptr : *findMember(product, "Parameters"));
  if (entries == nullptr) {
    return nullopt;
  }

  const auto normalize = [](const string& value) {
    string normalized;
    normalized.reserve(value.size());
    for (unsigned char ch : value) {
      if (isalnum(ch)) {
        normalized.push_back(static_cast<char>(tolower(ch)));
      }
    }
    return normalized;
  };

  for (const auto& entry : *entries) {
    const auto label = readFirstMember(entry, {"Parameter", "ParameterText"});
    const auto value = readParameterText(entry, label.value_or(""));
    if (!label.has_value() || !value.has_value()) {
      continue;
    }

    const auto normalizedLabel = normalize(*label);
    for (const auto* name : names) {
      const auto normalizedNeedle = normalize(name);
      if (normalizedLabel == normalizedNeedle || normalizedLabel.find(normalizedNeedle) != string::npos ||
          normalizedNeedle.find(normalizedLabel) != string::npos) {
        const auto trimmed = trimCopy(*value);
        if (!trimmed.empty()) {
          return trimmed;
        }
      }
    }
  }

  return nullopt;
}

string normalizeSearchKey(string value) {
  string normalized;
  normalized.reserve(value.size());
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      normalized.push_back(static_cast<char>(tolower(ch)));
    }
  }
  return normalized;
}

vector<string> tokenizeSearchKey(const string& value) {
  vector<string> tokens;
  string current;
  for (unsigned char ch : value) {
    if (isalnum(ch)) {
      current.push_back(static_cast<char>(tolower(ch)));
    } else if (!current.empty()) {
      tokens.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

bool tokenAppears(const string& haystack, const string& needle) {
  const auto normalizedHaystack = normalizeSearchKey(haystack);
  const auto normalizedNeedle = normalizeSearchKey(needle);
  return !normalizedNeedle.empty() && normalizedHaystack.find(normalizedNeedle) != string::npos;
}

int scoreSearchMatch(const SearchMatch& match, const string& query, bool exactBucket) {
  const auto normalizedQuery = normalizeSearchKey(query);
  const auto queryTokens = tokenizeSearchKey(query);

  const auto scoreCandidate = [&](const string& candidate, int exactScore, int containsScore) {
    int score = 0;
    const auto normalizedCandidate = normalizeSearchKey(candidate);
    if (normalizedCandidate.empty()) {
      return score;
    }
    if (!normalizedQuery.empty() && normalizedCandidate == normalizedQuery) {
      score += exactScore;
    } else if (!normalizedQuery.empty() &&
               (normalizedCandidate.find(normalizedQuery) != string::npos ||
                normalizedQuery.find(normalizedCandidate) != string::npos)) {
      score += containsScore;
    }
    for (const auto& token : queryTokens) {
      if (token.size() >= 2 && normalizedCandidate.find(token) != string::npos) {
        score += 8;
      }
    }
    return score;
  };

  int score = exactBucket ? 20 : 0;
  score += scoreCandidate(match.productNumber, 120, 80);
  score += scoreCandidate(match.manufacturerPartNumber, 140, 90);
  score += scoreCandidate(match.productDescription, 40, 25);
  score += scoreCandidate(match.detailedDescription, 30, 20);

  for (const auto& token : queryTokens) {
    if (token.size() >= 2 &&
        (tokenAppears(match.productNumber, token) || tokenAppears(match.manufacturerPartNumber, token))) {
      score += 15;
    }
    if (token.size() >= 2 && tokenAppears(match.productDescription, token)) {
      score += 4;
    }
  }

  return score;
}

optional<SearchMatch> extractSearchMatch(const JsonPtr& product) {
  SearchMatch match;
  match.manufacturerId = readPath(product, {"Manufacturer", "Id"}).value_or("");
  match.manufacturerPartNumber = readPath(product, {"ManufacturerProductNumber"}).value_or("");
  match.productDescription = readPath(product, {"Description", "ProductDescription"}).value_or("");
  match.detailedDescription = readPath(product, {"Description", "DetailedDescription"}).value_or("");

  const auto* variations = asArray(findMember(product, "ProductVariations") == nullptr ? nullptr : *findMember(product, "ProductVariations"));
  if (variations != nullptr) {
    for (const auto& variation : *variations) {
      if (const auto number = readFirstMember(variation, {"DigiKeyProductNumber"}); number.has_value() && !number->empty()) {
        match.productNumber = *number;
        return match;
      }
    }
  }

  if (const auto directNumber = readFirstMember(product, {"DigiKeyProductNumber"}); directNumber.has_value() &&
                                                                     !directNumber->empty()) {
    match.productNumber = *directNumber;
    return match;
  }

  if (const auto mpn = readPath(product, {"ManufacturerProductNumber"}); mpn.has_value() && !mpn->empty()) {
    match.productNumber = *mpn;
    return match;
  }

  return nullopt;
}

optional<SearchMatch> resolveSearchResult(const JsonPtr& root, const string& query) {
  optional<SearchMatch> bestMatch;
  int bestScore = 0;

  const auto considerMatches = [&](const JsonValue::Array* products, bool exactBucket) {
    if (products == nullptr) {
      return;
    }
    for (const auto& product : *products) {
      if (const auto match = extractSearchMatch(product); match.has_value()) {
        const int score = scoreSearchMatch(*match, query, exactBucket);
        if (score > bestScore) {
          bestScore = score;
          bestMatch = move(*match);
        }
      }
    }
  };

  considerMatches(asArray(findMember(root, "ExactMatches") == nullptr ? nullptr : *findMember(root, "ExactMatches")), true);
  considerMatches(asArray(findMember(root, "Products") == nullptr ? nullptr : *findMember(root, "Products")), false);

  if (bestScore <= 0) {
    return nullopt;
  }
  return bestMatch;
}

}  // namespace digikey_detail
}  // namespace inventatory

#endif
