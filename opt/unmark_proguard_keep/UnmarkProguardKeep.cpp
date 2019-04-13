/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <string.h>

#include "TypeSystem.h"
#include "UnmarkProguardKeep.h"
#include "Walkers.h"

void unmark_keep(const Scope& scope,
                 const std::vector<std::string>& package_list,
                 const std::vector<std::string>& supercls_list) {
  if (package_list.size() == 0 && supercls_list.size() == 0) {
    return;
  }
  std::unordered_set<const DexType*> superclasses;
  std::unordered_set<const DexType*> interface_list;
  for (auto& cls_name : supercls_list) {
    const DexType* supercls_type = DexType::get_type(cls_name);
    if (supercls_type) {
      DexClass* supercls = type_class(supercls_type);
      if (supercls && is_interface(supercls)) {
        interface_list.emplace(supercls_type);
      } else {
        superclasses.emplace(supercls_type);
      }
    }
  }
  TypeSystem ts(scope);
  // Unmark proguard keep rule for interface implementors like
  // "-keep class * extend xxx".
  for (const DexType* intf_type : interface_list) {
    for (const DexType* implementor : ts.get_implementors(intf_type)) {
      DexClass* implementor_cls = type_class(implementor);
      if (implementor_cls) {
        implementor_cls->rstate.force_unset_allowshrinking();
      }
    }
  }
  walk::parallel::classes(
      scope, [&ts, &superclasses, &package_list](DexClass* cls) {
        // Unmark proguard keep rule for classes under path from package list.
        for (const auto& package : package_list) {
          if (strstr(cls->get_name()->c_str(), package.c_str()) != nullptr) {
            cls->rstate.force_unset_allowshrinking();
            return;
          }
        }
        if (!is_interface(cls)) {
          // Unmark proguard keep for classes that extend class from superclass
          // list for proguard keep rule like "-keep class * extend xxx".
          const auto& parents_chain = ts.parent_chain(cls->get_type());
          if (parents_chain.size() <= 2) {
            // The class's direct super class is java.lang.Object, no need
            // to proceed.
            return;
          }
          // We only need to find class started at the second of parents_chain
          // because first is java.lang.Object, and end at second to last,
          // because last one is itself.
          for (uint32_t index = 1; index < parents_chain.size() - 1; ++index) {
            if (superclasses.find(parents_chain[index]) != superclasses.end()) {
              cls->rstate.force_unset_allowshrinking();
              return;
            }
          }
        }
      });
}

void UnmarkProguardKeepPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& /* conf */,
                                      PassManager& mgr) {
  auto scope = build_class_scope(stores);
  unmark_keep(scope, m_package_list, m_supercls_list);
}

static UnmarkProguardKeepPass s_pass;
