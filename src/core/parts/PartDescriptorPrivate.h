#pragma once

#include "core/parts/PartDescriptor.h"

#include <initializer_list>
#include <string>
#include <vector>

namespace inventatory::part_descriptor_detail {

struct LabelContext {
  std::string provider;
  std::vector<std::string> categories;
  std::string title;
  std::vector<Parameter> parameters;
};

std::string normalizedKey(const std::string& value);
std::vector<std::string> splitCategoryPath(const std::string& value);
void appendCategoryPath(std::vector<std::string>& target, const std::string& value);
LabelContext makeContext(const InventoryItem& item);
bool hasExactCategory(const LabelContext& context, std::initializer_list<const char*> names);
bool hasCategoryPhrase(const LabelContext& context, std::initializer_list<const char*> phrases);
bool hasParameter(const LabelContext& context, std::initializer_list<const char*> names,
                  std::initializer_list<const char*> valueFragments = {});
bool guardedTitleHas(const LabelContext& context, std::initializer_list<const char*> fragments);
std::string printAlias(std::string label);
PartDescriptor resolved(std::string purposeLabel, std::string printLabel, PartLabelSource source);

PartDescriptor discreteSemiconductorRule(const LabelContext& context);
PartDescriptor integratedCircuitRule(const LabelContext& context);
PartDescriptor specificCategoryMapping(const LabelContext& context);
PartDescriptor categoryMapping(const LabelContext& context);
PartDescriptor parentCategoryRule(const LabelContext& context);
PartDescriptor categoryAndParameterRule(const LabelContext& context);
PartDescriptor parentFamily(const LabelContext& context);
std::string fallbackCategory(const LabelContext& context);

}  // namespace inventatory::part_descriptor_detail
