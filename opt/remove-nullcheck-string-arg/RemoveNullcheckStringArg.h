/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

/**
 * Kotlin has null safety checks which adds runtime assertions. These assertions
 * takes the object and the identifier name which is holding the object
 * (parameter or field) as parameters.
 *
 * This pass modifies calls to these assertions (checkParameterIsNotNull and
 * checkExpressionValueIsNotNull) to a generated wrapper method calls such that
 * it reduces the string usage and code size. For example,
 * checkParameterIsNotNull like below will change from:
 *
 * (invoke-static (v0 v1)
 * "Lkotlin/jvm/internal/Intrinsics;.checkParameterIsNotNull:(Ljava/lang/Object;Ljava/lang/String;)V")
 * to:
 * (invoke-static (v0)
 * "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter(Ljava/lang/Object;)V")
 *
 * And, this pass will generate wrapper function called $WrCheckParameter in the
 * host class. checkParameter will in turn call checkExpressionValueIsNotNull
 * with empty string. Inliner will inline checkParameterIsNotNull into the
 * wrapper function.
 *
 */
class RemoveNullcheckStringArg : public Pass {

 public:
  using TransferMap = std::unordered_map<DexMethodRef*, DexMethod*>;
  using NewMethodSet = std::unordered_set<DexMethod*>;
  struct Stats {
    size_t null_check_insns_changed{0};
    Stats& operator+=(const Stats& that) {
      null_check_insns_changed += that.null_check_insns_changed;
      return *this;
    }
    /// Updates metrics tracked by \p mgr corresponding to these statistics.
    /// Simultaneously prints the statistics via TRACE.
    void report(PassManager& mgr) const;
  };

  RemoveNullcheckStringArg() : Pass("RemoveNullcheckStringArgPass") {}
  bool setup(TransferMap& transfer_map, NewMethodSet& new_methods);
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
  RemoveNullcheckStringArg::Stats change_in_cfg(
      cfg::ControlFlowGraph& cfg, const TransferMap& transfer_map);

 private:
  DexMethod* get_wrapper_method(const char* builtin_signature,
                                const char* wrapper_name,
                                const char* wrapper_signature);
};
