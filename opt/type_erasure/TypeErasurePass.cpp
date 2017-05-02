/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "TypeErasurePass.h"

#include "DexClass.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "VirtualScope.h"
#include "Walkers.h"
#include "ReferenceSet.h"
#include "TargetTypeHierarchy.h"
#include "InterfaceRemoval.h"

namespace {

void update_stores(
    const TypeSet& to_remove,
    Scope& scope,
    DexStoresVector& stores
) {
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

void TypeErasurePass::run_pass(
  DexStoresVector& stores, ConfigFiles&, PassManager& mgr
) {
  auto scope = build_class_scope(stores);
  auto gql_hierarchy =
      TargetTypeHierarchy::build_target_type_hierarchy(scope);
  TypeSet removable = check_interfaces(scope, gql_hierarchy);
  update_stores(removable, scope, stores);
  mgr.incr_metric("interface_removed", removable.size());
}

static TypeErasurePass s_pass;
