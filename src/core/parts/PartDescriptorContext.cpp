// Inventatory - Shared part descriptor context and matching helpers.

#include "PartDescriptorPrivate.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <utility>
#include <vector>

namespace inventatory::part_descriptor_detail {

using namespace std;

string normalizedKey(const string& value) {
  string key;
  key.reserve(value.size());
  for (const auto ch : value) {
    if (isalnum(static_cast<unsigned char>(ch))) {
      key.push_back(static_cast<char>(tolower(static_cast<unsigned char>(ch))));
    }
  }
  return key;
}

vector<string> splitCategoryPath(const string& value) {
  vector<string> categories;
  size_t start = 0;
  while (start < value.size()) {
    const auto slash = value.find(" / ", start);
    const auto dash = value.find(" - ", start);
    const auto separator = slash == string::npos ? dash : (dash == string::npos ? slash : min(slash, dash));
    const auto segment = trim(value.substr(start, separator == string::npos ? string::npos : separator - start));
    if (!segment.empty()) {
      categories.push_back(segment);
    }
    if (separator == string::npos) {
      break;
    }
    start = separator + 3;
  }
  return categories;
}

void appendCategoryPath(vector<string>& target, const string& value) {
  const auto trimmed = trim(value);
  if (trimmed.empty()) {
    return;
  }
  target.push_back(trimmed);
  for (const auto& segment : splitCategoryPath(trimmed)) {
    if (segment != trimmed) {
      target.push_back(segment);
    }
  }
}

LabelContext makeContext(const InventoryItem& item) {
  LabelContext context;
  context.provider = toLower(trim(item.vendorMetadata.provider));
  for (const auto& category : item.vendorMetadata.categoryPath) {
    appendCategoryPath(context.categories, category);
  }
  if (!trim(item.vendorMetadata.categoryId).empty()) {
    appendCategoryPath(context.categories, item.vendorMetadata.categoryId);
  }
  if (context.categories.empty()) {
    context.categories = splitCategoryPath(item.category);
  }
  if (!trim(item.category).empty()) {
    appendCategoryPath(context.categories, item.category);
  }
  context.title = trim(item.vendorMetadata.title);
  if (context.title.empty()) {
    context.title = item.partName;
  }
  context.parameters = item.vendorMetadata.parameters.empty() ? item.parameters : item.vendorMetadata.parameters;
  return context;
}

bool hasExactCategory(const LabelContext& context, initializer_list<const char*> names) {
  for (const auto& category : context.categories) {
    const auto categoryKey = normalizedKey(category);
    for (const auto* name : names) {
      if (categoryKey == normalizedKey(name)) {
        return true;
      }
    }
  }
  return false;
}

bool hasCategoryPhrase(const LabelContext& context, initializer_list<const char*> phrases) {
  for (const auto& category : context.categories) {
    const auto categoryKey = normalizedKey(category);
    for (const auto* phrase : phrases) {
      const auto phraseKey = normalizedKey(phrase);
      if (!phraseKey.empty() && categoryKey.find(phraseKey) != string::npos) {
        return true;
      }
    }
  }
  return false;
}

bool hasParameter(const LabelContext& context, initializer_list<const char*> names,
                  initializer_list<const char*> valueFragments) {
  for (const auto& parameter : context.parameters) {
    const auto parameterName = normalizedKey(parameter.name);
    bool nameMatched = false;
    for (const auto* name : names) {
      if (parameterName == normalizedKey(name)) {
        nameMatched = true;
        break;
      }
    }
    if (!nameMatched) {
      continue;
    }
    if (valueFragments.size() == 0) {
      return true;
    }
    const auto parameterValue = normalizedKey(parameter.value);
    for (const auto* fragment : valueFragments) {
      if (parameterValue.find(normalizedKey(fragment)) != string::npos) {
        return true;
      }
    }
  }
  return false;
}

bool guardedTitleHas(const LabelContext& context, initializer_list<const char*> fragments) {
  const auto titleKey = normalizedKey(context.title);
  for (const auto* fragment : fragments) {
    if (titleKey.find(normalizedKey(fragment)) != string::npos) {
      return true;
    }
  }
  return false;
}

string printAlias(string label) {
  // The printer scales label text to the available width. Keep the complete
  // deterministic description here instead of silently cutting it at 16
  // characters, which discarded meaningful subtype information.
  return trim(label);
}

PartDescriptor resolved(string purposeLabel, string printLabel, PartLabelSource source) {
  PartDescriptor descriptor;
  descriptor.purposeLabel = move(purposeLabel);
  descriptor.printLabel = printAlias(move(printLabel));
  descriptor.source = source;
  descriptor.usedFallback = source == PartLabelSource::Fallback;
  descriptor.shortDescription = descriptor.printLabel;
  return descriptor;
}

}  // namespace inventatory::part_descriptor_detail
