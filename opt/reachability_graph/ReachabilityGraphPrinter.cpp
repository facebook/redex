/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ReachabilityGraphPrinter.h"

#include "PassManager.h"
#include "ReachableObjects.h"

void ReachabilityGraphPrinterPass::run_pass(DexStoresVector& stores,
                                            ConfigFiles& /*cfg*/,
                                            PassManager& pm) {
  if (pm.no_proguard_rules()) {
    TRACE(RMU,
          1,
          "RemoveUnreachablePass not run because no "
          "ProGuard configuration was provided.");
    return;
  }

  // A bit ugly copy from RMU pass, but...
  std::unordered_set<const DexType*> ignore_string_literals_annos;
  for (const auto& name : m_ignore_string_literals) {
    const auto type = DexType::get_type(name.c_str());
    if (type != nullptr) {
      ignore_string_literals_annos.insert(type);
    }
  }

  auto reachables = compute_reachable_objects(
      stores, ignore_string_literals_annos, nullptr, true /*generate graph*/);

  // TODO: What if this pass is called multiple times? Need to add prefix or so.
  // PassManager already has this order information. I will do it soon.
  dump_reachability_graph(stores, reachables.retainers_of);
}

static ReachabilityGraphPrinterPass s_pass;
