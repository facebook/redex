/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "UnreferencedInterfaceRemovalPass.h"

#include "DexClass.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "VirtualScope.h"
#include "Walkers.h"
#include "ReferenceSet.h"
#include "TargetTypeHierarchy.h"

namespace {

TypeSet get_gql_interfaces(const Scope& scope) {
  auto ggql_hierarhcy = TargetTypeHierarchy::build_target_type_hierarchy(scope);
  auto gql_interfaces = ggql_hierarhcy.interfaces;
  TRACE(TERA, 2, " TERA collected %d interfaces \n", gql_interfaces.size());
  return gql_interfaces;
}

// Verifying if the candidate interfaces are safe to remove
TypeSet verify_interfaces(const Scope& scope, TypeSet& candidates) {
  TypeSet itfs;
  for (const auto& type : candidates) {
    auto cls = type_class(type);
    // Sanity check
    if (!can_delete(cls) ||
        cls->is_external()) {
      continue;
    }
    // Avoid static fields
    auto sfields = cls->get_sfields();
    if (!sfields.empty()) {
      continue;
    }
    itfs.insert(type);
  }
  // Exclude interfaces implemented by abstract classes.
  // Things could get complicated.
  for (const auto& cls : scope) {
    if (is_interface(cls) || !is_abstract(cls)) {
      continue;
    }
    // Only abstract classes
    auto impls = cls->get_interfaces()->get_type_list();
    for (const auto impl : impls) {
      if (itfs.count(impl) > 0) {
        itfs.erase(impl);
      }
    }
  }

  return itfs;
}

std::unordered_set<DexType*> removables(const TypeSet& unref, const std::deque<DexType*>& interfaces) {
  std::unordered_set<DexType*> res;
  for (const auto itf : interfaces) {
    if (unref.count(itf) > 0) {
      res.insert(itf);
    }
  }
  return res;
}

std::unordered_set<DexType*> removable_impls(
  const std::unordered_set<DexType*>& to_remove
) {
  std::unordered_set<DexType*> res;
  for (const auto rm : to_remove) {
    auto cls = type_class(rm);
    auto impls = cls->get_interfaces()->get_type_list();
    if (!impls.empty()) {
      res.insert(impls.begin(), impls.end());
    }
  }

  return res;
}

DexTypeList* get_updated_interface_list(
  const std::deque<DexType*>& old_list,
  const std::unordered_set<DexType*>& to_remove,
  const std::unordered_set<DexType*>& to_remove_impls
) {
  std::unordered_set<DexType*> new_list;
  for (const auto itf : old_list) {
    // Dedup
   if (to_remove.count(itf) > 0) {
     continue;
   }
   new_list.insert(itf);
  }
  new_list.insert(to_remove_impls.begin(), to_remove_impls.end());
  std::deque<DexType*> new_deque(new_list.begin(), new_list.end());
  return DexTypeList::make_type_list(std::move(new_deque));
}

void trace(
  DexClass* cls,
  const std::unordered_set<DexType*>& to_remove,
  DexTypeList* new_itfs
) {
  TRACE(TERA, 3, " TERA Removing unref interfaces on %s \n", SHOW(cls));
  TRACE(TERA, 3, " TERA   Removing interfaces ");
  for (const auto rem : to_remove) {
    TRACE(TERA, 3, " %s", SHOW(rem));
  }
  TRACE(TERA, 3, "\n");
  TRACE(TERA, 3, " TERA   old_list %d %s, new_list %d %s\n",
    cls->get_interfaces()->get_type_list().size(), SHOW(cls->get_interfaces()),
    new_itfs->get_type_list().size(), SHOW(new_itfs)
  );
}

void update_stores(const TypeSet& to_remove, Scope& scope, DexStoresVector& stores) {
  Scope tscope(scope);
  scope.clear();
  for (DexClass* cls : tscope) {
    if (to_remove.count(cls->get_type()) > 0) {
      TRACE(TERA, 3, " TERA Deleting class %s\n", SHOW(cls));
    } else {
      scope.push_back(cls);
    }
  }
  post_dexen_changes(scope, stores);
}

}

void UnreferencedInterfaceRemovalPass::run_pass(
  DexStoresVector& stores, ConfigFiles&, PassManager& mgr
) {
  auto scope = build_class_scope(stores);
  auto gql_interfaces = get_gql_interfaces(scope);
  gql_interfaces = verify_interfaces(scope, gql_interfaces);
  ReferenceSet ref_set(scope, get_gql_interfaces(scope));
  ref_set.print();
  TypeSet unref = ref_set.unrfs;

  for (const auto cls : scope) {
    const auto interfaces = cls->get_interfaces()->get_type_list();
    auto to_remove = removables(unref, interfaces);
    if (to_remove.empty()) {
      continue;
    }
    auto to_remove_impls = removable_impls(to_remove);
    const auto new_itfs = get_updated_interface_list(interfaces, to_remove, to_remove_impls);
    trace(cls, to_remove, new_itfs);
    cls->set_interfaces(new_itfs);
  }

  update_stores(unref, scope, stores);
  mgr.incr_metric("interface_removed", unref.size());
}

static UnreferencedInterfaceRemovalPass s_pass;
