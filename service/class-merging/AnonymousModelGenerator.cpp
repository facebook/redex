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

void discover_mergeable_anonymous_classes(const DexStoresVector& stores,
                                          size_t min_implementors,
                                          ModelSpec* merging_spec,
                                          PassManager* mgr) {
  auto root_store_classes =
      get_root_store_types(stores, merging_spec->include_primary_dex);
  const auto object_type = type::java_lang_Object();
  std::unordered_map<const DexType*, std::vector<const DexType*>> parents;
  for (auto* type : root_store_classes) {
    auto cls = type_class(type);
    if (!can_delete(cls)) {
      continue;
    }
    auto* itfs = cls->get_interfaces();
    if (is_interface(cls) || itfs->size() > 1 || !maybe_anonymous_class(cls) ||
        cls->get_clinit()) {
      continue;
    }
    auto super_cls = cls->get_super_class();
    if (itfs->size() == 1) {
      auto* intf = *itfs->begin();
      if (auto intf_def = type_class(intf)) {
        if (itfs->size() == 1) {
          parents[intf].push_back(cls->get_type());
          continue;
        }
      }
    } else {
      parents[super_cls].push_back(cls->get_type());
    }
  }
  for (const auto& pair : parents) {
    auto parent = pair.first;
    if (!merging_spec->exclude_types.count(parent) &&
        pair.second.size() >= min_implementors) {
      TRACE(CLMG,
            9,
            "Discover %sroot %s with %zu anonymous classes",
            (is_interface(type_class(parent)) ? "interface " : ""),
            SHOW(parent),
            pair.second.size());
      if (parent == object_type) {
        // TODO: Currently not able to merge classes that only extend
        // java.lang.Object.
        continue;
      }
      mgr->incr_metric("cls_" + show(parent), pair.second.size());
      merging_spec->roots.insert(parent);
      merging_spec->merging_targets.insert(pair.second.begin(),
                                           pair.second.end());
    }
  }
}

} // namespace class_merging
