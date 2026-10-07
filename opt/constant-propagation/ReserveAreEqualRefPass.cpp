/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReserveAreEqualRefPass.h"

#include "ConstantPropagationTransform.h"
#include "DexStructure.h"
#include "MethodUtil.h"
#include "PassManager.h"

void ReserveAreEqualRefPass::eval_pass(DexStoresVector&,
                                       ConfigFiles&,
                                       PassManager& mgr) {
  // Only reserve when the `areEqual -> Object.equals` rewrite is enabled and
  // `Intrinsics.areEqual` exists, whether defined or referenced. Otherwise, the
  // rewrite never introduces the `Object.equals` ref, and reserving would
  // needlessly reduce every dex's method ref capacity. Because the flag is set
  // from configuration before any pass runs, it is stable here.
  if (constant_propagation_transform_internal::enable_replacing_areequal &&
      method::kotlin_jvm_internal_Intrinsics_areEqual() != nullptr) {
    m_reserved_refs_handle = mgr.reserve_refs(name(),
                                              ReserveRefsInfo(/* frefs */ 0,
                                                              /* trefs */ 0,
                                                              /* mrefs */ 1));
  }
}

void ReserveAreEqualRefPass::run_pass(DexStoresVector&,
                                      ConfigFiles&,
                                      PassManager& mgr) {
  // Release the reservation (if one was taken) now that the last pass that can
  // perform the rewrite has run.
  if (m_reserved_refs_handle) {
    mgr.release_reserved_refs(*m_reserved_refs_handle);
    m_reserved_refs_handle = std::nullopt;
  }
}

static ReserveAreEqualRefPass s_pass;
