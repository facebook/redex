/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>

#include <sparta/ConstantAbstractDomain.h>
#include <sparta/PatriciaTreeMap.h>
#include <sparta/PatriciaTreeMapAbstractEnvironment.h>
#include <sparta/PatriciaTreeSet.h>
#include <sparta/PatriciaTreeSetAbstractDomain.h>

#include "BaseIRAnalyzer.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "IRCode.h"
#include "IROpcode.h"
#include "Lazy.h"
#include "LiveRange.h"
#include "MethodOverrideGraph.h"
#include "Resolver.h"
#include "Show.h"

namespace object_escape_analysis_impl {

DexMethod* resolve_invoke_method_if_unambiguous(
    const method_override_graph::Graph& method_override_graph,
    const IRInstruction* insn,
    const DexMethod* caller);

struct Callees {
  std::vector<DexMethod*> with_code;
  bool any_unknown{false};
  bool operator==(const Callees& other) const;
};

using CalleesKey = std::pair<DexMethod*, const DexType*>;
using CalleesKeyHash = boost::hash<CalleesKey>;
using CalleesCache =
    InsertOnlyConcurrentMap<CalleesKey, Callees, CalleesKeyHash>[2];

const Callees* resolve_invoke_callees(
    const method_override_graph::Graph& method_override_graph,
    const IRInstruction* insn,
    const DexMethod* caller,
    CalleesCache* callees_cache);

DexMethod* resolve_invoke_inlinable_callee(
    const method_override_graph::Graph& method_override_graph,
    const IRInstruction* insn,
    const DexMethod* caller,
    CalleesCache* callees_cache,
    const std::function<DexType*()>& inlinable_type_at_src_index_0_getter);

using Locations = std::vector<std::pair<DexMethod*, const IRInstruction*>>;

// Collect all allocation and invoke instructions, as well as non-virtual
// invocation dependencies.
void analyze_scope(
    const Scope& scope,
    const method_override_graph::Graph& method_override_graph,
    ConcurrentMap<DexType*, Locations>* new_instances,
    ConcurrentMap<DexMethod*, Locations>* single_callee_invokes,
    InsertOnlyConcurrentSet<DexMethod*>* multi_callee_invokes,
    ConcurrentMap<DexMethod*, std::unordered_set<DexMethod*>>* dependencies,
    CalleesCache* callees_cache);

// A benign method invocation can be ignored during the escape analysis.
bool is_benign(const DexMethodRef* method_ref);

// For each allocating instruction that escapes (not including returns), all
// uses by which it escapes.
using Escapes = std::unordered_map<const IRInstruction*, live_range::Uses>;

// For each object, we track which instruction might have allocated it:
// - new-instance, invoke-, and load-param-object instructions might represent
//   allocation points
// - NO_ALLOCATION is a value for which the allocation instruction is not known,
//   or it is not an object
using Domain = sparta::PatriciaTreeSetAbstractDomain<const IRInstruction*>;

// For each register that holds a relevant value, keep track of it.
using Environment = sparta::PatriciaTreeMapAbstractEnvironment<reg_t, Domain>;

struct MethodSummary {
  // A parameter is "benign" if a provided argument does not escape
  std::unordered_set<src_index_t> benign_params;

  // Whether the method returns nothing of interest, or only the result of a
  // unique instruction which allocates an object, or only a particular
  // parameter.
  std::variant<std::monostate, const IRInstruction*, src_index_t> returns;

  bool returns_allocation_or_param() const {
    return !std::holds_alternative<std::monostate>(returns);
  }

  const IRInstruction* allocation_insn() const {
    auto ptr = std::get_if<const IRInstruction*>(&returns);
    return ptr ? *ptr : nullptr;
  }

  std::optional<src_index_t> returned_param_index() const {
    auto ptr = std::get_if<src_index_t>(&returns);
    return ptr ? std::optional<src_index_t>(*ptr) : std::nullopt;
  }

  bool empty() const {
    return benign_params.empty() && !returns_allocation_or_param();
  }

  bool operator==(const MethodSummary& other) const {
    return benign_params == other.benign_params && returns == other.returns;
  }
};

using MethodSummaries = std::unordered_map<DexMethod*, MethodSummary>;

using MethodSummaryCache =
    InsertOnlyConcurrentMap<const Callees*, MethodSummary>;

const MethodSummary* resolve_invoke_method_summary(
    const method_override_graph::Graph& method_override_graph,
    const MethodSummaries& method_summaries,
    const IRInstruction* insn,
    const DexMethod* caller,
    CalleesCache* callees_cache,
    MethodSummaryCache* method_summary_cache);

// The analyzer computes...
// - which instructions allocate (new-instance, invoke-)
// - which allocations escape (and how)
// - which allocations return
class Analyzer final : public ir_analyzer::BaseIRAnalyzer<Environment> {
 public:
  explicit Analyzer(const method_override_graph::Graph& method_override_graph,
                    const std::unordered_set<DexClass*>& excluded_classes,
                    const MethodSummaries& method_summaries,
                    DexMethodRef* incomplete_marker_method,
                    DexMethod* method,
                    CalleesCache* callees_cache,
                    MethodSummaryCache* method_summary_cache);

  void analyze_instruction(const IRInstruction* insn,
                           Environment* current_state) const override;

  const Escapes& get_escapes() const { return m_escapes; }

  const std::unordered_set<const IRInstruction*>& get_returns() const {
    return m_returns;
  }

  // Returns set of new-instance and invoke- allocating instructions that do not
  // escape (or return).
  std::unordered_set<IRInstruction*> get_inlinables() const;

 private:
  const method_override_graph::Graph& m_method_override_graph;
  const std::unordered_set<DexClass*>& m_excluded_classes;
  const MethodSummaries& m_method_summaries;
  DexMethodRef* m_incomplete_marker_method;
  DexMethod* m_method;
  mutable Escapes m_escapes;
  mutable std::unordered_set<const IRInstruction*> m_returns;
  mutable CalleesCache* m_callees_cache;
  mutable MethodSummaryCache* m_method_summary_cache;

  bool is_incomplete_marker(const IRInstruction* insn) const {
    return insn->opcode() == OPCODE_INVOKE_STATIC &&
           insn->get_method() == m_incomplete_marker_method;
  }
};

MethodSummaries compute_method_summaries(
    const Scope& scope,
    const ConcurrentMap<DexMethod*, std::unordered_set<DexMethod*>>&
        dependencies,
    const method_override_graph::Graph& method_override_graph,
    const std::unordered_set<DexClass*>& excluded_classes,
    size_t* analysis_iterations,
    CalleesCache* callees_cache,
    MethodSummaryCache* method_summary_cache);

// For an inlinable new-instance or invoke- instruction, determine first
// resolved callee (if any), and (eventually) allocated type
std::pair<DexMethod*, DexType*> resolve_inlinable(
    const MethodSummaries& method_summaries,
    DexMethod* method,
    const IRInstruction* insn);

} // namespace object_escape_analysis_impl
