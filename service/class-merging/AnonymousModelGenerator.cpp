/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AnonymousModelGenerator.h"
#include "Model.h"
#include "Show.h"
#include "TypeUtil.h"
#include "Walkers.h"

namespace {

constexpr const char* LAMBDA_CLASS_NAME_PREFIX = "$$Lambda$";

/**
 * Return true if the name matches "$$Lambda$" or "$[0-9]".
 */
bool maybe_anonymous_class(const DexClass* cls) {
  const auto& name = cls->get_deobfuscated_name();
  auto pos = name.rfind('$');
  if (pos == std::string::npos) {
    return false;
  }
  pos++;
  return (pos < name.size() && name[pos] >= '0' && name[pos] <= '9') ||
         name.find(LAMBDA_CLASS_NAME_PREFIX) != std::string::npos;
}
} // namespace

namespace class_merging {

/**
 * Analyze type hierarchy to find anonymous classes to merge.
 */
void discover_mergeable_anonymous_classes(const Scope& scope,
                                          ModelSpec* merging_spec) {
  merging_spec->name = "Anonymous Classes";
  merging_spec->class_name_prefix = "Ano";
  // TODO(fengliu): The limit should be configurable.
  constexpr const size_t min_implementor_size = 500;
  const auto object_type = type::java_lang_Object();
  // TODO(fengliu): Change the vector to number of types if no more analysis
  // needed.
  std::unordered_map<DexType*, std::vector<DexType*>> interface_to_implementors;
  walk::classes(scope, [&](const DexClass* cls) {
    if (is_interface(cls) || cls->get_super_class() != object_type ||
        cls->get_interfaces()->size() != 1 || !maybe_anonymous_class(cls)) {
      return;
    }
    if (!can_delete(cls)) {
      return;
    }
    auto* intf = *cls->get_interfaces()->begin();
    if (auto intf_def = type_class(intf)) {
      if (intf_def->get_interfaces()->size() == 0) {
        interface_to_implementors[intf].push_back(cls->get_type());
        return;
      }
    }
  });
  for (const auto& pair : interface_to_implementors) {
    auto intf = pair.first;
    if (!merging_spec->exclude_types.count(intf) &&
        pair.second.size() >= min_implementor_size) {
      TRACE(CLMG, 9, "discovered new root %s", SHOW(pair.first));
      merging_spec->roots.insert(pair.first);
    }
  }
}
} // namespace class_merging
