/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mutex>

#include "ConcurrentContainers.h"
#include "PassManager.h"
#include "TypeSystem.h"

namespace remove_unused_args {

class RemoveArgs {
 public:
  struct MethodStats {
    size_t method_params_removed_count{0};
    size_t methods_updated_count{0};
  };
  struct PassStats {
    size_t method_params_removed_count{0};
    size_t methods_updated_count{0};
    size_t callsite_args_removed_count{0};
  };

  RemoveArgs(const Scope& scope) : m_scope(scope), m_type_system(scope){};
  RemoveArgs::PassStats run();
  std::deque<uint16_t> compute_live_args(
      DexMethod* method,
      size_t num_args,
      std::vector<IRInstruction*>* dead_insns);

 private:
  std::mutex m_lock;
  const Scope& m_scope;
  TypeSystem m_type_system;
  ConcurrentMap<DexMethod*, std::deque<uint16_t>> m_live_arg_idxs_map;
  std::unordered_map<DexTypeList*, size_t> m_renamed_virtual_arg_lists;

  std::deque<DexType*> get_live_arg_type_list(
      DexMethod* method, const std::deque<uint16_t>& live_arg_idxs);
  bool update_method_signature(DexMethod* method,
                               const std::deque<uint16_t>& live_args);
  MethodStats update_meths_with_unused_args();
  size_t update_callsite(IRInstruction* instr);
  size_t update_callsites();
};

class RemoveUnusedArgsPass : public Pass {
 public:
  RemoveUnusedArgsPass() : Pass("RemoveUnusedArgsPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager& mgr) override;
};

} // namespace remove_unused_args
