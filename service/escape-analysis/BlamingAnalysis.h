/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "DexClass.h"
#include "IRInstruction.h"
#include "IntervalDomain.h"
#include "LiftedDomain.h"
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
  using Value = sparta::LiftedDomain<BlameDomain>;

  /* Model of: Value -> (Nat x {Instruction}) */
  using Domain =
      sparta::PatriciaTreeMapAbstractEnvironment<const IRInstruction*, Value>;

  static void set_may_escape(const IRInstruction* ptr,
                             const IRInstruction* blamed,
                             Domain* dom) {
    dom->update(ptr, [blamed](const Value& val) {
      if (val.lowered().is_bottom()) {
        // Cannot escape an unallocated value.
        return Value::bottom();
      }

      auto cpy = val;
      cpy.lowered().add(blamed);
      return cpy;
    });
  }

  static void set_fresh(const IRInstruction* ptr, Domain* dom) {
    dom->set(ptr, allocated());
  }

  static bool may_have_escaped(const Domain&, const IRInstruction*) {
    // Unimplemented
    not_reached();
  }

  static Value unallocated() { return Value::lifted(BlameDomain::bottom()); }

  static Value allocated() {
    auto parts = std::make_tuple(CountDomain::finite(0, 0), InstructionSet());
    return Value::lifted(BlameDomain(std::move(parts)));
  }
};

using Environment = EnvironmentWithStoreImpl<BlameStore>;

class FixpointIterator final : public ir_analyzer::BaseIRAnalyzer<Environment> {
 public:
  explicit FixpointIterator(const cfg::ControlFlowGraph& cfg,
                            std::unordered_set<const IRInstruction*> allocators,
                            std::unordered_set<DexMethodRef*> safe_method_refs,
                            std::unordered_set<DexString*> safe_method_names);

  void analyze_instruction(const IRInstruction* insn,
                           Environment* env) const override;

 private:
  // The instructions whose results we track for escapes.
  std::unordered_set<const IRInstruction*> m_allocators;

  // Methods that are assumed not to escape any of their parameters.
  std::unordered_set<DexMethodRef*> m_safe_method_refs;

  // Methods that are assumed not to escape any of their parameters, identified
  // by their names.
  std::unordered_set<DexString*> m_safe_method_names;

  /* Returns true if and only if `insn` is considered an allocator */
  bool is_allocator(const IRInstruction* insn) const;

  /* Returns true if and only if `method` is assumed to be safe. */
  bool is_safe_method(DexMethodRef* method) const;
};

/* A method that should be treated as safe */
struct SafeMethod {
  // NOLINTNEXTLINE
  /* implicit */ SafeMethod(DexMethodRef* method_ref)
      : type(SafeMethod::ByRef), method_ref(method_ref) {}

  // NOLINTNEXTLINE
  /* implicit */ SafeMethod(DexString* method_name)
      : type(SafeMethod::ByName), method_name(method_name) {}

  enum { ByRef, ByName } type;
  union {
    DexMethodRef* method_ref;
    DexString* method_name;
  };
};

/*
 * A facade over BlameStore::Domain to simplify querying the results of the
 * analysis.  Interface exposes a mapping from allocating instructions to
 * a value that summarises the analysis' findings for that allocator (see
 * BlameMap::Value).
 */
class BlameMap {
 public:
  class Value {
   public:
    explicit Value(BlameStore::Value value) : m_value(std::move(value)) {}

    /* Whether or not the allocator was reached by the analysis */
    bool allocated() const {
      return !m_value.is_bottom() && !m_value.lowered().is_bottom();
    }

    /*
     * The upper and lower-bounds on the number of times allocations from this
     * site could have escaped, assuming the allocator was reached.
     */
    const CountDomain& escape_counts() const {
      assert(allocated() && "Only allocated values can escape");
      return m_value.lowered().get<0>();
    }

    /*
     * The set of instructions to blame for escapes of values allocated from
     * this site, assuming the allocator was reached.
     */
    const InstructionSet& to_blame() const {
      assert(allocated() && "Only allocated values can escape");
      return m_value.lowered().get<1>();
    }

    /*
     * True if and only if it is possible for values from this allocation site
     * to escape multiple times during one trace of execution.  Only a valid
     * question to ask for reached allocators.
     */
    bool may_multi_escape() const {
      const auto& count = escape_counts();
      return !count.is_bottom() && count.upper_bound() > 1;
    }

   private:
    const BlameStore::Value m_value;
  };

  explicit BlameMap(BlameStore::Domain domain) : m_domain(std::move(domain)) {}

  size_t size() const { return m_domain.size(); }

  /*
   * Returns results of analysis for the allocation site `alloc`.  Requests for
   * the results of allocation sites that were not tracked or reached by the
   * analysis will both result in a result that indicates no allocations
   * occurred.
   */
  Value get(const IRInstruction* alloc) const {
    return Value{m_domain.get(alloc)};
  }

 private:
  BlameStore::Domain m_domain;
};

/*
 * Analyse the escapes of objects in `cfg` allocated by the `allocator`
 * instructions.
 *
 * The analysis requires that the ControlFlowGraph it is given has a unique
 * exit block and will introduce one if one does not already exist.
 *
 * The analysis assumes that all instructions in `allocators` have a
 * destination register, and it is the value in that register that could be
 * escaped.  This is tested lazily (i.e. only if the instruction is reached).
 *
 * Only methods identified by `safe_methods` are assumed not to escape any of
 * their parameters.  Similarly, only invokes identified as allocators are
 * assumed not to escape their return values.
 *
 * Returns a mapping from allocating instructions to the following information:
 *
 *  - Whether it was reached by the analysis.
 *  - The set of instructions that could potentially escape its values, assuming
 *    it was reached.
 *  - An approximation (as an interval) of the number of times one of its
 *    instances could escape on any given trace through the CFG, assuming it
 *    was reached.
 *
 */
BlameMap analyze_escapes(cfg::ControlFlowGraph& cfg,
                         std::unordered_set<const IRInstruction*> allocators,
                         std::initializer_list<SafeMethod> safe_methods = {});

inline bool FixpointIterator::is_allocator(const IRInstruction* insn) const {
  return m_allocators.count(insn);
}

inline bool FixpointIterator::is_safe_method(DexMethodRef* method) const {
  return method != nullptr && (m_safe_method_refs.count(method) ||
                               m_safe_method_names.count(method->get_name()));
}

} // namespace blaming
} // namespace local_pointers
