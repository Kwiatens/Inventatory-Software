// Inventatory - Hardware Inventory Management System
// Core inventory string, identifier, and item helpers.

#include "core/InventoryInternals.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace inventatory {

using namespace std;

namespace {

vector<string> splitTokensRespectingQuotes(const string& query) {
  vector<string> tokens;
  string current;
  bool inQuotes = false;

  for (char ch : query) {
    if (ch == '"') {
      inQuotes = !inQuotes;
      continue;
    }

    if (!inQuotes && isspace(static_cast<unsigned char>(ch))) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }

    current.push_back(ch);
  }

  if (!current.empty()) {
    tokens.push_back(current);
  }

  return tokens;
}

}  // namespace

time_t nowEpoch() {
  return time(nullptr);
}

string sanitizeIdPart(const string& value) {
  string output;
  output.reserve(value.size());
  for (char ch : value) {
    if (isalnum(static_cast<unsigned char>(ch))) {
      output.push_back(static_cast<char>(tolower(static_cast<unsigned char>(ch))));
    } else if (!output.empty() && output.back() != '-') {
      output.push_back('-');
    }
  }

  while (!output.empty() && output.back() == '-') {
    output.pop_back();
  }

  if (output.empty()) {
    output = "item";
  }
  return output;
}

bool InventoryItem::hasMissingMetadata() const {
  return partName.empty() || manufacturer.empty() || category.empty() || digikeyPartNumber.empty() ||
         datasheetUrl.empty() || productUrl.empty();
}

string InventoryItem::searchableText() const {
  ostringstream out;
  out << partName << ' ' << manufacturer << ' ' << category << ' ' << location << ' ' << notes << ' '
      << digikeyPartNumber << ' ' << datasheetUrl << ' ' << productUrl << ' ' << sku << ' ' << machineCode << ' '
      << inventatoryId << ' ' << syncStatus << ' ' << labelOverride << ' ' << vendorMetadata.provider << ' '
      << vendorMetadata.providerProductNumber << ' ' << vendorMetadata.manufacturerPartNumber << ' '
      << vendorMetadata.title << ' ' << vendorMetadata.detailedDescription;

  for (const auto& tag : tags) {
    out << ' ' << tag;
  }

  for (const auto& parameter : parameters) {
    out << ' ' << parameter.name << ':' << parameter.value;
  }

  for (const auto& vendorCategory : vendorMetadata.categoryPath) {
    out << ' ' << vendorCategory;
  }
  for (const auto& parameter : vendorMetadata.parameters) {
    out << ' ' << parameter.name << ':' << parameter.value;
  }

  return toLower(out.str());
}

string trim(const string& value) {
  const auto begin = find_if_not(value.begin(), value.end(), [](unsigned char ch) {
    return isspace(ch) != 0;
  });
  const auto end = find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
    return isspace(ch) != 0;
  }).base();

  if (begin >= end) {
    return {};
  }

  return string(begin, end);
}

string toLower(string value) {
  transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(tolower(ch));
  });
  return value;
}

string toUpper(string value) {
  transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(toupper(ch));
  });
  return value;
}

string nowTimestampString(time_t value) {
  tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &value);
#else
  localtime_r(&value, &tm);
#endif
  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &tm);
  return buffer;
}

string makeId() {
  const auto stamp = chrono::system_clock::now().time_since_epoch().count();
  mt19937_64 rng(static_cast<mt19937_64::result_type>(stamp));
  uniform_int_distribution<unsigned long long> dist;
  ostringstream out;
  out << hex << setw(10) << setfill('0') << (stamp & 0xfffffffffULL) << '-' << setw(10) << setfill('0')
      << (dist(rng) & 0xfffffffffULL);
  return out.str();
}


string join(const vector<string>& values, char delimiter) {
  ostringstream out;
  for (size_t index = 0; index < values.size(); ++index) {
    if (index > 0) {
      out << delimiter;
    }
    out << values[index];
  }
  return out.str();
}

vector<string> split(const string& value, char delimiter) {
  vector<string> values;
  string current;
  istringstream input(value);

  while (getline(input, current, delimiter)) {
    current = trim(current);
    if (!current.empty()) {
      values.push_back(current);
    }
  }

  return values;
}

vector<string> tokenizeQuery(const string& query) {
  return splitTokensRespectingQuotes(trim(query));
}

bool containsInsensitive(string_view haystack, string_view needle) {
  if (needle.empty()) {
    return true;
  }

  string loweredHaystack(haystack);
  string loweredNeedle(needle);
  loweredHaystack = toLower(move(loweredHaystack));
  loweredNeedle = toLower(move(loweredNeedle));
  return loweredHaystack.find(loweredNeedle) != string::npos;
}

}  // namespace inventatory
