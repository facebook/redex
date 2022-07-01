/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

namespace cfg {
class ControlFlowGraph;
} // namespace cfg

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
 * (invoke-static (v0, v1)
 * "Lkotlin/jvm/internal/Intrinsics;.$WrCheckParameter(Ljava/lang/Object;I)V")
 *
 * And, this pass will generate wrapper function called $WrCheckParameter and
 * $WrCheckExpression in the host class. $WrcheckParameter will in turn call
 * checkParameterIsNotNull with a string converted from index or
 * $WrCheckExpression will call checkExpressionValueIsNotNull with an empty
 * string. Inliner will inline checkParameterIsNotNull into the wrapper
 * function.
 *
 */
class RemoveNullcheckStringArg : public Pass {

 public:
  // Records the wrapper method for assertions with a boolean indicating if the
  // wrapper method takes the index of the parameter as an argument. If the
  // wrapper is going to print the index of the param as a string, we will
  // construct a string from the index with additional information as part of
  // the wrapper method and print that as part of the trace.
  using TransferMap =
      std::unordered_map<DexMethodRef*, std::pair<DexMethod*, bool>>;
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
  RemoveNullcheckStringArg::Stats change_in_cfg(cfg::ControlFlowGraph& cfg,
                                                const TransferMap& transfer_map,
                                                bool is_virtual);

 private:
  /* If the \p wrapper_signature is already present or if the function being
   * wrapped does not exist or if creation of new method fails, return nullptr.
   * Otherwise create a method in the same class as in \p
   * builtin_signature with a new name \p wrapper_name. */
  DexMethod* get_wrapper_method(const char* wrapper_signature,
                                const char* wrapper_name,
                                DexMethodRef* builtin);
  /* If the \p wrapper_signature, that also takes int index, is already present
   * or if the function being wrapped does not exist or if creation of new
   * method fails, return nullptr. Otherwise create a method in the same class
   * as in \p builtin_signature with a new name \p wrapper_name. */
  DexMethod* get_wrapper_method_with_int_index(const char* wrapper_signature,
                                               const char* wrapper_name,
                                               DexMethodRef* builtin);
};
