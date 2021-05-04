/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AnonymousModelGenerator.h"
#include "Model.h"
#include "PassManager.h"
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
                                          size_t min_implementors,
                                          ModelSpec* merging_spec,
                                          PassManager* mgr) {
  const auto object_type = type::java_lang_Object();
  std::unordered_map<DexType*, size_t> interfaces;
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
        interfaces[intf] += 1;
        return;
      }
    }
  });
  for (const auto& pair : interfaces) {
    auto intf = pair.first;
    if (!merging_spec->exclude_types.count(intf) &&
        pair.second >= min_implementors) {
      mgr->incr_metric("impls_" + show(intf), pair.second);
      TRACE(CLMG, 9, "discovered new root %s", SHOW(pair.first));
      merging_spec->roots.insert(pair.first);
    }
  }
}

} // namespace class_merging
