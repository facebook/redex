/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>
#include <utility>
#include <vector>

#include <sparta/AbstractDomain.h>
#include <sparta/PatriciaTreeMapAbstractEnvironment.h>

#include "ControlFlow.h"
#include "DeterministicContainers.h"
#include "IRInstruction.h"
#include "LocalPointersAnalysis.h"

namespace stringbuilder_analysis {

/*
 * The sequence of StringBuilder method calls that have been invoked on a given
 * StringBuilder instance.
 */
using BuilderState = std::vector<const IRInstruction*>;

class BuilderValue final : public sparta::AbstractValue<BuilderValue> {
 public:
  BuilderValue() = default;

  void clear() { m_state.clear(); }

  sparta::AbstractValueKind kind() const {
    return sparta::AbstractValueKind::Value;
  }

  bool leq(const BuilderValue& other) const { return equals(other); }

  bool equals(const BuilderValue& other) const {
    return m_state == other.m_state;
  }

  sparta::AbstractValueKind join_with(const BuilderValue& other) {
    if (equals(other)) {
      return sparta::AbstractValueKind::Value;
    }
    return sparta::AbstractValueKind::Top;
  }

  sparta::AbstractValueKind widen_with(const BuilderValue& other) {
    return join_with(other);
  }

  sparta::AbstractValueKind meet_with(const BuilderValue& other) {
    if (equals(other)) {
      return sparta::AbstractValueKind::Value;
    }
    return sparta::AbstractValueKind::Bottom;
  }

  sparta::AbstractValueKind narrow_with(const BuilderValue& other) {
    return meet_with(other);
  }

  const BuilderState& state() const { return m_state; }

  void add_operation(const IRInstruction* insn) { m_state.emplace_back(insn); }

 private:
  BuilderState m_state;
};

class BuilderDomain final
    : public sparta::AbstractDomainScaffolding<BuilderValue, BuilderDomain> {
 public:
  // Inherit constructors from AbstractDomainScaffolding.
  using AbstractDomainScaffolding::AbstractDomainScaffolding;

  // Constructor inheritance is buggy in some versions of gcc, hence the
  // redefinition of the default constructor.
  BuilderDomain() {}

  void add_operation(const IRInstruction* insn) {
    if (kind() == sparta::AbstractValueKind::Value) {
      get_value()->add_operation(insn);
    }
  }

  std::optional<BuilderState> state() const {
    return (kind() == sparta::AbstractValueKind::Value)
               ? std::optional<BuilderState>(get_value()->state())
               : std::nullopt;
  }
};

/*
 * A model of StringBuilder objects stored on the heap.
 */
class BuilderStore {
 public:
  using Domain =
      sparta::PatriciaTreeMapAbstractEnvironment<const IRInstruction*,
                                                 BuilderDomain>;

  static void set_may_escape(const IRInstruction* ptr,
                             const IRInstruction* /* blame */,
                             Domain* dom) {
    dom->set(ptr, BuilderDomain::top());
  }

  static void set_fresh(const IRInstruction* ptr, Domain* dom) {
    dom->set(ptr, BuilderDomain());
  }

  static bool may_have_escaped(const Domain& dom, const IRInstruction* ptr) {
    return dom.get(ptr).is_top();
  }
};

using Environment = local_pointers::EnvironmentWithStoreImpl<BuilderStore>;

class FixpointIterator final : public ir_analyzer::BaseIRAnalyzer<Environment> {
 public:
  explicit FixpointIterator(const cfg::ControlFlowGraph& cfg);

  void analyze_instruction(const IRInstruction* insn,
                           Environment* env) const override;

 private:
  bool is_eligible_init(const DexMethodRef*) const;
  bool is_eligible_append(const DexMethodRef*) const;

  const DexType* m_stringbuilder;
  UnorderedSet<const DexType*> m_immutable_types;
  const DexMethodRef* m_stringbuilder_no_param_init;
  const DexMethodRef* m_stringbuilder_init_with_string;
  const DexString* m_append_str;
};

using InstructionSet = UnorderedSet<const IRInstruction*>;

using BuilderStateMap =
    std::vector<std::pair<const IRInstruction*, BuilderState>>;

InstructionSet find_tostring_instructions(const cfg::ControlFlowGraph& cfg,
                                          const DexMethodRef* tostring_method);

BuilderStateMap gather_builder_states(
    const cfg::ControlFlowGraph& cfg,
    const InstructionSet& eligible_tostring_instructions);

} // namespace stringbuilder_analysis
