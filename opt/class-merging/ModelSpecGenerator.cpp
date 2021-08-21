/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ModelSpecGenerator.h"
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

/**
 * The methods may have associated keeping rules, exclude the classes if they or
 * their methods are not deleteable. For example, methods annotated
 * with @android.webkit.JavascriptInterface are invoked reflectively, and we
 * should keep them.
 *
 * Why not check the constructors and fields?
 * Some of the constructors and fields are marked as non-deletable but the class
 * is apparently mergeable.
 */
bool can_delete_class_or_nonctor_methods(const DexClass* cls) {
  if (!can_delete(cls)) {
    return false;
  }
  auto& vmethods = cls->get_vmethods();
  if (std::any_of(vmethods.begin(), vmethods.end(),
                  [](const DexMethod* m) { return !can_delete(m); })) {
    return false;
  }
  auto& dmethods = cls->get_dmethods();
  if (std::any_of(dmethods.begin(), dmethods.end(), [](const DexMethod* m) {
        return !is_constructor(m) && !can_delete(m);
      })) {
    return false;
  }
  return true;
}

bool is_from_allowed_packages(
    const std::unordered_set<std::string>& allowed_packages,
    const DexClass* cls) {
  if (allowed_packages.empty()) {
    return true;
  }
  const auto& name = cls->get_deobfuscated_name();
  for (auto& prefix : allowed_packages) {
    if (boost::starts_with(name, prefix)) {
      return true;
    }
  }
  return false;
}

} // namespace

namespace class_merging {

void find_all_mergeables_and_roots(const TypeSystem& type_system,
                                   const Scope& scope,
                                   size_t global_min_count,
                                   ModelSpec* merging_spec) {
  std::unordered_map<const DexTypeList*, std::vector<const DexType*>>
      intfs_implementors;
  std::unordered_map<const DexType*, std::vector<const DexType*>>
      parent_children;
  TypeSet throwable;
  type_system.get_all_children(type::java_lang_Throwable(), throwable);
  for (const auto* cls : scope) {
    auto cur_type = cls->get_type();
    if (is_interface(cls) || is_abstract(cls) || cls->rstate.is_generated() ||
        cls->get_clinit() || throwable.count(cur_type)) {
      continue;
    }
    TypeSet children;
    type_system.get_all_children(cur_type, children);
    if (!children.empty()) {
      continue;
    }
    if (!can_delete_class_or_nonctor_methods(cls)) {
      continue;
    }
    auto* intfs = cls->get_interfaces();
    auto super_cls = cls->get_super_class();
    if (super_cls != type::java_lang_Object()) {
      parent_children[super_cls].push_back(cur_type);
    } else if (intfs->size()) {
      intfs_implementors[intfs].push_back(cur_type);
    } else {
      // TODO: Investigate error P444184021 when merging simple classes without
      // interfaces.
    }
  }
  for (const auto& pair : parent_children) {
    auto parent = pair.first;
    auto& children = pair.second;
    if (children.size() >= global_min_count) {
      TRACE(CLMG,
            9,
            "Discover root %s with %zu child classes",
            SHOW(parent),
            children.size());
      merging_spec->roots.insert(parent);
      merging_spec->merging_targets.insert(children.begin(), children.end());
    }
  }
  for (const auto& pair : intfs_implementors) {
    auto intfs = pair.first;
    auto& implementors = pair.second;
    if (implementors.size() >= global_min_count) {
      TRACE(CLMG,
            9,
            "Discover interface root %s with %zu implementors",
            SHOW(intfs),
            pair.second.size());
      auto first_implementor = type_class(implementors[0]);
      merging_spec->roots.insert(first_implementor->get_super_class());
      merging_spec->merging_targets.insert(implementors.begin(),
                                           implementors.end());
    }
  }
  TRACE(CLMG, 9, "Discover %zu mergeables from %zu roots",
        merging_spec->merging_targets.size(), merging_spec->roots.size());
}

void discover_mergeable_anonymous_classes(
    const DexStoresVector& stores,
    const std::unordered_set<std::string>& allowed_packages,
    size_t min_implementors,
    ModelSpec* merging_spec,
    PassManager* mgr) {
  auto root_store_classes =
      get_root_store_types(stores, merging_spec->include_primary_dex);
  const auto object_type = type::java_lang_Object();
  std::unordered_map<const DexType*, std::vector<const DexType*>> parents;
  for (auto* type : root_store_classes) {
    auto cls = type_class(type);
    auto* itfs = cls->get_interfaces();
    if (is_interface(cls) || itfs->size() > 1 || !maybe_anonymous_class(cls) ||
        cls->get_clinit() || !is_from_allowed_packages(allowed_packages, cls)) {
      continue;
    }
    if (!can_delete_class_or_nonctor_methods(cls)) {
      continue;
    }
    auto super_cls = cls->get_super_class();
    if (itfs->size() == 1) {
      auto* intf = *itfs->begin();
      if (type_class(intf)) {
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
      if (is_interface(type_class(parent))) {
        auto first_mergeable = type_class(pair.second[0]);
        merging_spec->roots.insert(first_mergeable->get_super_class());
      } else {
        merging_spec->roots.insert(parent);
      }
      merging_spec->merging_targets.insert(pair.second.begin(),
                                           pair.second.end());
    }
  }
}

} // namespace class_merging
