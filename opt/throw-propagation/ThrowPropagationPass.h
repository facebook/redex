/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexStore.h"
#include "MethodOverrideGraph.h"
#include "Pass.h"

class ThrowPropagationPass : public Pass {
 public:
  struct Config {
    bool debug{false};
    std::unordered_set<const DexType*> blocklist;
  };

  struct Stats {
    int unreachable_instruction_count{0};
    int throws_inserted{0};

    Stats& operator+=(const Stats&);
  };

 private:
  Config m_config;

  static bool is_no_return_method(const Config& config, DexMethod* method);

 public:
  ThrowPropagationPass() : Pass("ThrowPropagationPass") {}

  void bind_config() override;

  static std::unordered_set<DexMethod*> get_no_return_methods(
      const Config& config, const Scope& scope);

  static Stats run(
      const Config& config,
      const std::unordered_set<DexMethod*>& no_return_methods,
      const method_override_graph::Graph& graph,
      IRCode* code,
      std::unordered_set<DexMethod*>* no_return_methods_checked = nullptr);
  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
