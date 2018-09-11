/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveUnreachable.h"

#include "PassManager.h"

void RemoveUnreachablePass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& /*cfg*/,
                                     PassManager& pm) {
  if (pm.no_proguard_rules()) {
    TRACE(RMU,
          1,
          "RemoveUnreachablePass not run because no "
          "ProGuard configuration was provided.");
    return;
  }

  int num_ignore_check_strings = 0;
  auto reachables = reachability::compute_reachable_objects(
      stores, m_ignore_sets, &num_ignore_check_strings);
  reachability::ObjectCounts before = reachability::count_objects(stores);
  TRACE(RMU, 1, "before: %lu classes, %lu fields, %lu methods\n",
        before.num_classes, before.num_fields, before.num_methods);
  reachability::sweep(stores, *reachables);
  reachability::ObjectCounts after = reachability::count_objects(stores);
  TRACE(RMU, 1, "after: %lu classes, %lu fields, %lu methods\n",
        after.num_classes, after.num_fields, after.num_methods);
  pm.incr_metric("num_ignore_check_strings", num_ignore_check_strings);
  pm.incr_metric("classes_removed", before.num_classes - after.num_classes);
  pm.incr_metric("fields_removed", before.num_fields - after.num_fields);
  pm.incr_metric("methods_removed", before.num_methods - after.num_methods);
}

static RemoveUnreachablePass s_pass;
