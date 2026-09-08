// Inventatory - Bounded DigiKey JSON parser and structural accessors.

#include "platform/digikey/DigiKeyApiPrivate.h"

#ifdef _WIN32

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace inventatory {
using namespace std;
namespace digikey_detail {

const JsonValue* asValue(const JsonPtr& value) {
  return value ? value.get() : nullptr;
}

const JsonValue::Object* asObject(const JsonPtr& value) {
  if (const auto* json = asValue(value); json != nullptr) {
    return get_if<JsonValue::Object>(&json->data);
  }
  return nullptr;
}

const JsonValue::Array* asArray(const JsonPtr& value) {
  if (const auto* json = asValue(value); json != nullptr) {
    return get_if<JsonValue::Array>(&json->data);
  }
  return nullptr;
}

string valueText(const JsonPtr& value) {
  if (const auto* json = asValue(value); json != nullptr) {
    if (const auto* text = get_if<string>(&json->data)) {
      if (text->size() > kMaximumDigiKeyFieldBytes) return {};
      return trimCopy(*text);
    }
    if (const auto* number = get_if<JsonValue::Number>(&json->data)) {
      if (number->text.size() > kMaximumDigiKeyFieldBytes) return {};
      return number->text;
    }
    if (const auto* flag = get_if<bool>(&json->data)) {
      return *flag ? "true" : "false";
    }
  }
  return {};
}

const JsonPtr* findMember(const JsonPtr& object, const string& key) {
  const auto* jsonObject = asObject(object);
  if (jsonObject == nullptr) {
    return nullptr;
  }
  const auto it = jsonObject->find(key);
  return it == jsonObject->end() ? nullptr : &it->second;
}

optional<string> readPath(const JsonPtr& root, initializer_list<const char*> path) {
  JsonPtr current = root;
  for (const auto* element : path) {
    const auto* next = findMember(current, element);
    if (next == nullptr) {
      return nullopt;
    }
    current = *next;
  }
  const auto text = valueText(current);
  if (text.empty()) {
    return nullopt;
  }
  return text;
}

optional<string> readStringPath(const JsonPtr& root, initializer_list<const char*> path) {
  JsonPtr current = root;
  for (const auto* element : path) {
    const auto* next = findMember(current, element);
    if (next == nullptr) return nullopt;
    current = *next;
  }
  const auto* json = asValue(current);
  const auto* text = json == nullptr ? nullptr : get_if<string>(&json->data);
  if (text == nullptr || text->empty() || text->size() > kMaximumDigiKeyFieldBytes) return nullopt;
  return trimCopy(*text);
}

optional<string> readFirstMember(const JsonPtr& root, initializer_list<const char*> keys) {
  for (const auto* key : keys) {
    if (const auto* member = findMember(root, key); member != nullptr) {
      const auto text = valueText(*member);
      if (!text.empty()) {
        return text;
      }
    }
  }
  return nullopt;
}

void appendCategoryPathFromNode(const JsonPtr& node, vector<string>& path) {
  if (node == nullptr || path.size() >= kMaximumCategoryPathEntries) {
    return;
  }
  if (const auto name = readFirstMember(node, {"Name", "CategoryName"}); name.has_value()) {
    if (path.empty() || path.back() != *name) {
      path.push_back(*name);
    }
  }
  if (const auto* children = asArray(findMember(node, "Children") == nullptr ? nullptr : *findMember(node, "Children"));
      children != nullptr) {
    for (const auto& child : *children) {
      if (path.size() >= kMaximumCategoryPathEntries) break;
      appendCategoryPathFromNode(child, path);
    }
  }
}

vector<string> extractCategoryPath(const JsonPtr& product) {
  for (const auto* key : {"Category", "ProductCategory"}) {
    if (const auto* node = findMember(product, key); node != nullptr) {
      vector<string> path;
      appendCategoryPathFromNode(*node, path);
      if (!path.empty()) {
        return path;
      }
    }
  }

  if (const auto* taxonomy = asArray(findMember(product, "LimitedTaxonomy") == nullptr
                                        ? nullptr
                                        : *findMember(product, "LimitedTaxonomy"));
      taxonomy != nullptr && !taxonomy->empty()) {
    vector<string> path;
    for (const auto& node : *taxonomy) {
      appendCategoryPathFromNode(node, path);
    }
    if (!path.empty()) {
      return path;
    }
  }

  if (const auto* classifications = asArray(findMember(product, "Classifications") == nullptr
                                               ? nullptr
                                               : *findMember(product, "Classifications"));
      classifications != nullptr && !classifications->empty()) {
    vector<string> path;
    for (const auto& node : *classifications) {
      appendCategoryPathFromNode(node, path);
    }
    if (!path.empty()) {
      return path;
    }
  }

  return {};
}

}  // namespace digikey_detail
}  // namespace inventatory

#endif
