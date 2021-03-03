/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "GlobalTypeAnalysisPass.h"
#include "Pass.h"
#include "RemoveUnreachable.h"

class TypeAnalysisAwareRemoveUnreachablePass
    : public RemoveUnreachablePassBase {
 public:
  TypeAnalysisAwareRemoveUnreachablePass()
      : RemoveUnreachablePassBase("TypeAnalysisAwareRemoveUnreachablePass") {}

  void set_analysis_usage(AnalysisUsage& au) const override {
    au.add_required<GlobalTypeAnalysisPass>();
    au.set_preserve_all();
  }

  std::unique_ptr<reachability::ReachableObjects> compute_reachable_objects(
      const DexStoresVector& stores,
      PassManager& pm,
      int* num_ignore_check_strings,
      bool emit_graph_this_run,
      bool remove_no_argument_constructors) override;
};
