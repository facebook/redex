/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "ControlFlow.h"
#include "DexClass.h"
#include "IRCode.h"

using ConstTypeHashSet = std::unordered_set<const DexType*>;

class IRInstruction;

namespace builder_pattern {

namespace impl {

// Forward declaration.
class Analyzer;
class InstructionToEnvMap;

} // namespace impl

using InstantiationToUsage =
    std::unordered_map<const IRInstruction*,
                       std::vector<cfg::InstructionIterator>>;

class BuilderAnalysis final {
 public:
  ~BuilderAnalysis();

  BuilderAnalysis(const ConstTypeHashSet& builder_types,
                  const ConstTypeHashSet& excluded_builder_types,
                  DexMethod* method);

  void run_analysis();

  void print_usage();

  void populate_usage();

  bool has_usage() const { return !m_usage.empty(); }
  const InstantiationToUsage& get_usage() { return m_usage; }
  size_t get_num_usages() const { return m_usage.size(); }
  size_t get_total_num_usages() const { return m_total_usages; }
  void set_num_inline_iterations(size_t num) { m_num_inline_iterations = num; }
  size_t get_num_inline_iterations() const { return m_num_inline_iterations; }

  std::unordered_map<IRInstruction*, DexType*>
  get_vinvokes_to_this_infered_type();
  std::unordered_set<IRInstruction*> get_all_inlinable_insns();

  ConstTypeHashSet get_instantiated_types();

  ConstTypeHashSet non_removable_types();

  ConstTypeHashSet get_escaped_types_from_invokes(
      const std::unordered_set<IRInstruction*>& insns) const;

 private:
  std::unique_ptr<impl::Analyzer> m_analyzer;
  InstantiationToUsage m_usage;
  std::unordered_set<const IRInstruction*> m_excluded_instantiation;
  const ConstTypeHashSet& m_builder_types;
  const ConstTypeHashSet& m_excluded_builder_types;
  std::unique_ptr<impl::InstructionToEnvMap> m_insn_to_env;
  std::unordered_map<const IRInstruction*, const DexType*>
      m_invoke_to_builder_instance;

  DexMethod* m_method;
  size_t m_total_usages;
  size_t m_num_inline_iterations{0};
  bool m_accept_excluded;

  void update_stats();
  ConstTypeHashSet escape_types();
};

} // namespace builder_pattern
