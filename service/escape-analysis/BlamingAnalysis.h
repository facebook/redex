/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "IRInstruction.h"
#include "IntervalDomain.h"
#include "LocalPointersAnalysis.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "PatriciaTreeSetAbstractDomain.h"
#include "ReducedProductAbstractDomain.h"

/*
 * A variant of local escape analysis.  In addition to tracking which abstract
 * instances escape, also tracks:
 *
 *  - Which instructions are responsible for escaping them.
 *  - How many times an instance could escape, in any trace of execution
 *    through the code.
 *
 * e.g. given code as follows:
 *
 *     Object o = new Object();
 *     while (cond) {
 *       foo(o);
 *     }
 *
 * The analysis will state [0] that `o` escapes potentially infinitely many
 * times, blaming the call to `foo`, whereas given the example below:
 *
 *     Object p = new Object();
 *     if (cond) {
 *       foo(p);
 *     } else {
 *       bar(p);
 *     }
 *
 * `p` will be seen to escape exactly once [0], with blame shared with either
 * `foo` or `bar`.
 *
 * [0] Technically in these examples, the analysis will count the constructor
 *     invocation as an escape as well, unless it has been marked as "safe". For
 *     the purposes of the example, assume that is the case.
 */

namespace local_pointers {
namespace blaming {

/* Models the number of times a value escapes. */
using CountDomain = sparta::IntervalDomain<int8_t>;

/* Models the instructions that are to blame for escaping a value. */
using InstructionSet =
    sparta::PatriciaTreeSetAbstractDomain<const IRInstruction*>;

/* Models a value that could escape: CountDomain x InstructionSet */
class BlameDomain final
    : public sparta::ReducedProductAbstractDomain<BlameDomain,
                                                  CountDomain,
                                                  InstructionSet> {
 public:
  using Base = sparta::
      ReducedProductAbstractDomain<BlameDomain, CountDomain, InstructionSet>;
  using Base::ReducedProductAbstractDomain;

  static void reduce_product(std::tuple<CountDomain, InstructionSet>&) {}

  const CountDomain& escape_counts() const { return Base::template get<0>(); }

  const InstructionSet& to_blame() const { return Base::template get<1>(); }

  bool may_multi_escape() const {
    const auto& count = escape_counts();
    return !count.is_bottom() && count.upper_bound() > 1;
  }

  void add(const IRInstruction* blamed) {
    Base::template apply<0>([](CountDomain* count) { *count += 1; });
    Base::template apply<1>(
        [blamed](InstructionSet* insns) { insns->add(blamed); });
  }
};

/*
 * A model of the heap can track which instructions are to blame for a value
 * escaping and how many times during execution it escapes.
 */
class BlameStore {
 public:
  /* Model of: Value -> (Nat x {Instruction}) */
  using Domain =
      sparta::PatriciaTreeMapAbstractEnvironment<const IRInstruction*,
                                                 BlameDomain>;

  static void set_may_escape(const IRInstruction* ptr,
                             const IRInstruction* blamed,
                             Domain* dom) {
    dom->update(ptr, [blamed](const BlameDomain& bdom) {
      auto cpy = bdom;
      cpy.add(blamed);
      return cpy;
    });
  }

  static void set_fresh(const IRInstruction* ptr, Domain* dom) {
    dom->set(ptr, BlameDomain({CountDomain::finite(0, 0), InstructionSet()}));
  }

  static bool may_have_escaped(const Domain& dom, const IRInstruction* ptr) {
    return !dom.get(ptr).is_bottom();
  }
};

using Environment = EnvironmentWithStoreImpl<BlameStore>;

class FixpointIterator final : public ir_analyzer::BaseIRAnalyzer<Environment> {
 public:
  explicit FixpointIterator(
      const cfg::ControlFlowGraph& cfg,
      std::unordered_set<const IRInstruction*> allocators);

  void analyze_instruction(const IRInstruction* insn,
                           Environment* env) const override;

 private:
  // The instructions whose results we track for escapes.
  std::unordered_set<const IRInstruction*> m_allocators;

  /* Returns true if and only if `insn` is considered an allocator */
  bool is_allocator(const IRInstruction* insn) const;
};

/*
 * Analyse the escapes of objects in `cfg` allocated by the `allocator`
 * instructions, which are themselves assumed to be in `cfg`.
 *
 * The analysis requires that the ControlFlowGraph it is given has a unique
 * exit block and will introduce one if one does not already exist.
 *
 * The analysis assumes that all instructions in `allocators` have a
 * destination register, and it is the value in that register that could be
 * escaped.  This is tested lazily (i.e. only if the instruction is reached).
 *
 * Returns a mapping from allocating instructions to an instance of
 * `BlameDomain` which conveys two kinds of information about potential escapes
 * for instances allocated by that instruction:
 *
 *  - The set of instructions that could potentially be blamed for the escape.
 *  - An approximation (as an interval) of the number of times an instance
 *    could escape on any given trace through the CFG.
 *
 * An instruction in `allocators` will only appear as a key in the output if it
 * is reachable in `cfg`.
 *
 */
BlameStore::Domain analyze_escapes(
    cfg::ControlFlowGraph& cfg,
    std::unordered_set<const IRInstruction*> allocators);

inline bool FixpointIterator::is_allocator(const IRInstruction* insn) const {
  return m_allocators.count(insn);
}

} // namespace blaming
} // namespace local_pointers
