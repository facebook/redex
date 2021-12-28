/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "DexClass.h"

class IRInstruction;

namespace builder_pattern {

namespace impl {

// Forward declaration.
class Analyzer;
class InstructionToEnvMap;

} // namespace impl

using InstantiationToUsage =
    std::unordered_map<const IRInstruction*, std::vector<const IRInstruction*>>;

class BuilderAnalysis final {
 public:
  ~BuilderAnalysis();

  BuilderAnalysis(const std::unordered_set<const DexType*>& types,
                  const std::unordered_set<const DexType*>& exclude,
                  DexMethod* method);

  void run_analysis();

  void print_usage();

  void populate_usage();

  bool has_usage() const { return !m_usage.empty(); }
  const InstantiationToUsage& get_usage() { return m_usage; }
  size_t get_num_usages() const { return m_usage.size(); }
  size_t get_total_num_usages() const { return m_total_usages; }

  std::unordered_map<IRInstruction*, DexType*>
  get_vinvokes_to_this_infered_type();
  std::unordered_set<IRInstruction*> get_all_inlinable_insns();

  std::unordered_set<const DexType*> get_instantiated_types(
      std::unordered_set<const IRInstruction*>* insns = nullptr);

  std::unordered_set<DexType*> non_removable_types();

 private:
  std::unique_ptr<impl::Analyzer> m_analyzer;
  InstantiationToUsage m_usage;
  std::unordered_set<const IRInstruction*> m_excluded_instantiation;
  const std::unordered_set<const DexType*>& m_types;
  const std::unordered_set<const DexType*>& m_exclude;
  std::unique_ptr<impl::InstructionToEnvMap> m_insn_to_env;

  DexMethod* m_method;
  size_t m_total_usages;
  bool m_accept_excluded;

  void update_stats();
  std::unordered_set<DexType*> escape_types();
};

} // namespace builder_pattern
