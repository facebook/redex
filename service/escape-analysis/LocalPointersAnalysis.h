/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "DexClass.h"
#include "MonotonicFixpointIterator.h"
#include "ObjectDomain.h"
#include "PatriciaTreeMapAbstractPartition.h"
#include "PatriciaTreeSetAbstractDomain.h"
#include "ReducedProductAbstractDomain.h"

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

using reg_t = uint32_t;

constexpr reg_t RESULT_REGISTER = std::numeric_limits<reg_t>::max();

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
   * Set :reg to contain the single abstract pointer :insn, which points to an
   * escaping value.
   */
  void set_escaping_pointer(reg_t reg, const IRInstruction* insn) {
    apply<0>(
        [&](PointerEnvironment* penv) { penv->set(reg, PointerSet(insn)); });
    apply<1>([&](HeapDomain* heap) {
      heap->set(insn, EscapeDomain(EscapeState::MAY_ESCAPE));
    });
  }

  /*
   * Set :reg to contain the single abstract pointer :insn, which points to a
   * non-escaping value.
   */
  void set_nonescaping_pointer(reg_t reg, const IRInstruction* insn) {
    apply<0>(
        [&](PointerEnvironment* penv) { penv->set(reg, PointerSet(insn)); });
    apply<1>([&](HeapDomain* heap) {
      heap->set(insn, EscapeDomain(EscapeState::NOT_ESCAPED));
    });
  }
};

inline bool is_alloc_opcode(IROpcode op) {
  return op == OPCODE_NEW_INSTANCE || op == OPCODE_NEW_ARRAY ||
         op == OPCODE_FILLED_NEW_ARRAY;
}

class FixpointIterator final
    : public sparta::MonotonicFixpointIterator<cfg::GraphInterface,
                                               Environment> {
 public:
  FixpointIterator(const cfg::ControlFlowGraph& cfg)
      : MonotonicFixpointIterator(cfg) {}

  void analyze_node(const NodeId& block, Environment* env) const override {
    for (auto& mie : InstructionIterable(block)) {
      analyze_instruction(mie.insn, env);
    }
  }

  void analyze_instruction(const IRInstruction* insn, Environment* env) const;

  Environment analyze_edge(const EdgeId&,
                           const Environment& entry_env) const override {
    return entry_env;
  }
};

} // namespace local_pointers
