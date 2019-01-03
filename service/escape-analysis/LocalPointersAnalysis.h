/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <ostream>

#include "BaseIRAnalyzer.h"
#include "CallGraph.h"
#include "ConcurrentContainers.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "ObjectDomain.h"
#include "PatriciaTreeMapAbstractPartition.h"
#include "PatriciaTreeSetAbstractDomain.h"
#include "ReducedProductAbstractDomain.h"
#include "S_Expression.h"

/*
 * This analysis identifies heap values that are allocated within a given
 * method and never escape it. Specifically, it determines all the pointers
 * that a given register may contain, and figures out which of these pointers
 * must not escape.
 *
 * Note that we do not model instance fields or array elements, so any values
 * written to them will be treated as escaping, even if the containing object
 * does not escape the method.
 */

namespace local_pointers {

using reg_t = ir_analyzer::register_t;

using PointerSet = sparta::PatriciaTreeSetAbstractDomain<const IRInstruction*>;

using PointerEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<reg_t, PointerSet>;

using HeapDomain =
    sparta::PatriciaTreeMapAbstractPartition<const IRInstruction*,
                                             EscapeDomain>;

class Environment final
    : public sparta::ReducedProductAbstractDomain<Environment,
                                                  PointerEnvironment,
                                                  HeapDomain> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  static void reduce_product(
      const std::tuple<PointerEnvironment, HeapDomain>&) {}

  const PointerEnvironment& get_pointer_environment() const {
    return ReducedProductAbstractDomain::get<0>();
  }

  const HeapDomain& get_heap() const {
    return ReducedProductAbstractDomain::get<1>();
  }

  PointerSet get_pointers(reg_t reg) const {
    return get_pointer_environment().get(reg);
  }

  EscapeDomain get_pointee(const IRInstruction* insn) const {
    return get_heap().get(insn);
  }

  void set_pointers(reg_t reg, PointerSet pset) {
    apply<0>([&](PointerEnvironment* penv) { penv->set(reg, pset); });
  }

  /*
   * Consider all pointers that may be contained in this register to point to
   * escaping values.
   */
  void set_may_escape(reg_t reg) {
    auto pointers = get_pointers(reg);
    if (!pointers.is_value()) {
      return;
    }
    apply<1>([&](HeapDomain* heap) {
      for (const auto* pointer : pointers.elements()) {
        heap->set(pointer, EscapeDomain(EscapeState::MAY_ESCAPE));
      }
    });
  }

  /*
   * Set :reg to contain the single abstract pointer :insn, which points to a
   * value with :escape_state.
   */
  void set_pointer_at(reg_t reg,
                      const IRInstruction* insn,
                      EscapeState escape_state) {
    apply<0>(
        [&](PointerEnvironment* penv) { penv->set(reg, PointerSet(insn)); });
    apply<1>(
        [&](HeapDomain* heap) { heap->set(insn, EscapeDomain(escape_state)); });
  }
};

inline bool is_alloc_opcode(IROpcode op) {
  return op == OPCODE_NEW_INSTANCE || op == OPCODE_NEW_ARRAY ||
         op == OPCODE_FILLED_NEW_ARRAY;
}

using ParamSet = sparta::PatriciaTreeSetAbstractDomain<uint16_t>;

struct EscapeSummary {
  // The elements of this set represent the indexes of the src registers that
  // escape.
  std::unordered_set<uint16_t> escaping_parameters;

  // The indices of the src registers that are returned. This is useful for
  // modeling methods that return `this`, though it is also able to model the
  // general case. It is a set instead of a single value since a method may
  // return different values depending on its inputs.
  //
  // Note that if only some of the returned values are parameters, this will be
  // set to Top. A non-extremal value indicates that the return value must be
  // an element of the set.
  ParamSet returned_parameters;

  EscapeSummary() = default;

  EscapeSummary(std::initializer_list<uint16_t> l) : escaping_parameters(l) {}

  EscapeSummary(ParamSet ps, std::initializer_list<uint16_t> l)
      : escaping_parameters(l), returned_parameters(ps) {}

  static EscapeSummary from_s_expr(const sparta::s_expr&);
};

std::ostream& operator<<(std::ostream& o, const EscapeSummary& summary);

sparta::s_expr to_s_expr(const EscapeSummary&);

using InvokeToSummaryMap =
    std::unordered_map<const IRInstruction*, EscapeSummary>;

class FixpointIterator final : public ir_analyzer::BaseIRAnalyzer<Environment> {
 public:
  FixpointIterator(
      const cfg::ControlFlowGraph& cfg,
      InvokeToSummaryMap invoke_to_summary_map = InvokeToSummaryMap())
      : ir_analyzer::BaseIRAnalyzer<Environment>(cfg),
        m_invoke_to_summary_map(invoke_to_summary_map) {}

  void analyze_instruction(IRInstruction* insn,
                           Environment* env) const override;

 private:
  // A map of the invoke instructions in the analyzed method to their respective
  // summaries. If an invoke instruction is not present in the method, we treat
  // it as an unknown method which could do anything (so all arguments may
  // escape).
  //
  // By taking this map as a parameter -- instead of trying to resolve callsites
  // ourselves -- we are able to switch easily between different call graph
  // construction strategies.
  InvokeToSummaryMap m_invoke_to_summary_map;
};

using FixpointIteratorMap =
    ConcurrentMap<const DexMethodRef*, FixpointIterator*>;

struct FixpointIteratorMapDeleter final {
  void operator()(FixpointIteratorMap*);
};

using FixpointIteratorMapPtr =
    std::unique_ptr<FixpointIteratorMap, FixpointIteratorMapDeleter>;

using SummaryMap = std::unordered_map<const DexMethodRef*, EscapeSummary>;

using SummaryCMap = ConcurrentMap<const DexMethodRef*, EscapeSummary>;

/*
 * Analyze all methods in scope, making sure to analyze the callees before
 * their callers.
 *
 * If a non-null SummaryCMap pointer is passed in, it will get populated
 * with the escape summaries of the methods in scope.
 */
FixpointIteratorMapPtr analyze_scope(const Scope&,
                                     const call_graph::Graph&,
                                     SummaryCMap* = nullptr);

EscapeSummary get_escape_summary(const FixpointIterator& fp_iter,
                                 const IRCode& code);

} // namespace local_pointers
