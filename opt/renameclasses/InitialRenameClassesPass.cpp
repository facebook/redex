/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InitialRenameClassesPass.h"
#include "ClassHierarchy.h"
#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "PassManager.h"
#include "ReachableClasses.h"
#include "RenameClassesV2.h"
#include "Trace.h"

void InitialRenameClassesPass::initial_rename_classes(Scope& scope,
                                                      PassManager& mgr) {
  for (auto clazz : scope) {
    auto dtype = clazz->get_type();
    auto oldname = dtype->get_name();

    if (clazz->rstate.is_force_rename()) {
      TRACE(RENAME, 2, "ComputeRename:Forced renamed: '%s'", oldname->c_str());
    } else if (clazz->rstate.is_dont_rename()) {
      mgr.incr_metric("num_initialize_renamable_false", 1);
      continue;
    }
    clazz->rstate.set_force_rename();
    mgr.incr_metric("num_initialize_renamable_true", 1);
  }
}

void InitialRenameClassesPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles&,
                                        PassManager& mgr) {
  auto rename_classes_pass =
      static_cast<RenameClassesPassV2*>(mgr.find_pass("RenameClassesPassV2"));
  if (!rename_classes_pass) {
    // No need to run InitialRenameClassesPass.
    return;
  }

  // Run rename_classes_pass->eval_classes_post to cover those classes generated
  // after pass evaluation.
  auto scope = build_class_scope(stores);
  ClassHierarchy class_hierarchy = build_type_hierarchy(scope);
  rename_classes_pass->eval_classes_post(scope, class_hierarchy, mgr);

  initial_rename_classes(scope, mgr);
}

static InitialRenameClassesPass s_pass;
