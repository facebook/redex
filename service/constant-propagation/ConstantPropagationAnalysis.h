/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <limits>
#include <unordered_set>
#include <utility>

#include <sparta/MonotonicFixpointIterator.h>

#include "BaseIRAnalyzer.h"
#include "ConcurrentContainers.h"
#include "ConstantEnvironment.h"
#include "IRCode.h"
#include "InstructionAnalyzer.h"
#include "KotlinNullCheckMethods.h"
#include "MethodUtil.h"

class DexMethodRef;

namespace constant_propagation {

boost::optional<size_t> get_null_check_object_index(
    const IRInstruction* insn,
    const std::unordered_set<DexMethodRef*>& kotlin_null_check_assertions);

namespace intraprocedural {

class FixpointIterator final
    : public ir_analyzer::BaseEdgeAwareIRAnalyzer<ConstantEnvironment> {
 public:
  /*
   * The fixpoint iterator takes an optional WholeProgramState argument that
   * it will use to determine the static field values and method return values.
   */
  FixpointIterator(const cfg::ControlFlowGraph& cfg,
                   InstructionAnalyzer<ConstantEnvironment> insn_analyzer,
                   bool imprecise_switches = false);

  void clear_switch_succ_cache() const { m_switch_succs.clear(); }

 protected:
  void analyze_instruction_normal(const IRInstruction* insn,
                                  ConstantEnvironment* env) const override;

  void analyze_if(const IRInstruction* insn,
                  cfg::Edge* const& edge,
                  ConstantEnvironment* env) const override;

  void analyze_switch(const IRInstruction* insn,
                      cfg::Edge* const& edge,
                      ConstantEnvironment* env) const override;

  void analyze_no_throw(const IRInstruction* insn,
                        ConstantEnvironment* env) const override;

 private:
  using SwitchSuccs = std::unordered_map<int32_t, uint32_t>;
  mutable std::unordered_map<cfg::Block*, SwitchSuccs> m_switch_succs;
  InstructionAnalyzer<ConstantEnvironment> m_insn_analyzer;
  const std::unordered_set<DexMethodRef*>& m_kotlin_null_check_assertions;
  const DexMethodRef* m_redex_null_check_assertion;

  const bool m_imprecise_switches;

  const SwitchSuccs& find_switch_succs(cfg::Block* block) const {
    auto it = m_switch_succs.find(block);
    if (it == m_switch_succs.end()) {
      std::vector<int32_t> keys;
      for (auto succ : block->succs()) {
        if (succ->type() == cfg::EDGE_BRANCH) {
          keys.push_back(*succ->case_key());
        }
      }
      std::sort(keys.begin(), keys.end());
      SwitchSuccs switch_succs;
      for (auto key : keys) {
        switch_succs.emplace(key, switch_succs.size());
      }
      it = m_switch_succs.emplace(block, std::move(switch_succs)).first;
    }
    return it->second;
  }

  bool has_switch_consecutive_case_keys(cfg::Block* block,
                                        int32_t min_case_key,
                                        int32_t max_case_key) const {
    always_assert(min_case_key <= max_case_key);
    const auto& succs = find_switch_succs(block);
    auto min_it = succs.find(min_case_key);
    if (min_it == succs.end()) {
      return false;
    }
    auto max_it = succs.find(max_case_key);
    if (max_it == succs.end()) {
      return false;
    }
    always_assert(min_it->second <= max_it->second);
    uint32_t cases_keys = max_it->second - min_it->second;
    int64_t n = (int64_t)max_case_key - min_case_key;
    always_assert(n >= 0 && n < std::numeric_limits<uint32_t>::max());
    return cases_keys == (uint32_t)n;
  }
};

} // namespace intraprocedural

/*
 * Propagates primitive constants and handles simple arithmetic. Also defines
 * an analyze_default implementation that simply sets any modified registers
 * to Top. This sub-analyzer should typically be the last one in any list of
 * combined sub-analyzers.
 */
class PrimitiveAnalyzer final
    : public InstructionAnalyzerBase<PrimitiveAnalyzer, ConstantEnvironment> {
 public:
  static bool analyze_default(const IRInstruction* insn,
                              ConstantEnvironment* env);

  static bool analyze_const(const IRInstruction* insn,
                            ConstantEnvironment* env);

  static bool analyze_move(const IRInstruction* insn, ConstantEnvironment* env);

  static bool analyze_move_result(const IRInstruction* insn,
                                  ConstantEnvironment* env);

  static bool analyze_cmp(const IRInstruction* insn, ConstantEnvironment* env);

  static bool analyze_binop_lit(const IRInstruction* insn,
                                ConstantEnvironment* env);

  static bool analyze_binop(const IRInstruction* insn,
                            ConstantEnvironment* env);

  static bool analyze_check_cast(const IRInstruction* insn,
                                 ConstantEnvironment* env);

  static bool analyze_instance_of(const IRInstruction* insn,
                                  ConstantEnvironment* env);
};

class InjectionIdAnalyzer final
    : public InstructionAnalyzerBase<InjectionIdAnalyzer, ConstantEnvironment> {
 public:
  static bool analyze_injection_id(const IRInstruction* insn,
                                   ConstantEnvironment* env);
};

// This is the most common use of constant propagation, so we define this alias
// for our convenience.
using ConstantPrimitiveAnalyzer =
    InstructionAnalyzerCombiner<PrimitiveAnalyzer>;

/*
 * Defines default analyses of opcodes that have the potential to let
 * heap-allocated values escape. It sets the escaped values to Top.
 *
 * This Analyzer should typically be used in sequence with other Analyzers
 * that allocate values on the abstract heap.
 */
class HeapEscapeAnalyzer final
    : public InstructionAnalyzerBase<HeapEscapeAnalyzer, ConstantEnvironment> {
 public:
  static bool analyze_sput(const IRInstruction* insn, ConstantEnvironment* env);
  static bool analyze_iput(const IRInstruction* insn, ConstantEnvironment* env);
  static bool analyze_aput(const IRInstruction* insn, ConstantEnvironment* env);
  static bool analyze_invoke(const IRInstruction* insn,
                             ConstantEnvironment* env);
  static bool analyze_filled_new_array(const IRInstruction* insn,
                                       ConstantEnvironment* env);
};

/*
 * Handle non-escaping arrays.
 *
 * This Analyzer should typically be used followed by the
 * HeapEscapeAnalyzer in a combined analysis -- LocalArrayAnalyzer
 * only handles the creation and mutation of array values, but does not account
 * for how they may escape.
 */
class LocalArrayAnalyzer final
    : public InstructionAnalyzerBase<LocalArrayAnalyzer, ConstantEnvironment> {
 public:
  static bool analyze_new_array(const IRInstruction* insn,
                                ConstantEnvironment* env);

  static bool analyze_aget(const IRInstruction* insn, ConstantEnvironment* env);

  static bool analyze_aput(const IRInstruction* insn, ConstantEnvironment* env);

  static bool analyze_fill_array_data(const IRInstruction* insn,
                                      ConstantEnvironment* env);
};

/*
 * Handle static fields in <clinit> methods. Since class method_initializers
 * must (in most cases) complete running before any other piece of code can
 * modify these fields, we can treat them as non-escaping while analyzing these
 * methods.
 */
class ClinitFieldAnalyzer final
    : public InstructionAnalyzerBase<ClinitFieldAnalyzer,
                                     ConstantEnvironment,
                                     DexType* /* class_under_init */> {
 public:
  static bool analyze_sget(const DexType* class_under_init,
                           const IRInstruction* insn,
                           ConstantEnvironment* env);

  static bool analyze_sput(const DexType* class_under_init,
                           const IRInstruction* insn,
                           ConstantEnvironment* env);

  static bool analyze_invoke(const DexType* class_under_init,
                             const IRInstruction* insn,
                             ConstantEnvironment* env);
};

/*
 * Handle instance fields in <init> methods.
 */
class InitFieldAnalyzer final
    : public InstructionAnalyzerBase<InitFieldAnalyzer,
                                     ConstantEnvironment,
                                     DexType* /* class_under_init */> {
 public:
  static bool analyze_iget(const DexType* class_under_init,
                           const IRInstruction* insn,
                           ConstantEnvironment* env);

  static bool analyze_iput(const DexType* class_under_init,
                           const IRInstruction* insn,
                           ConstantEnvironment* env);

  static bool analyze_invoke(const DexType* class_under_init,
                             const IRInstruction* insn,
                             ConstantEnvironment* env);
};

struct EnumFieldAnalyzerState {
  // Construction of this object is expensive because get_method acquires a
  // global lock. `get` creates a singleton once and reuses it.
  static const EnumFieldAnalyzerState& get();

  const DexMethod* enum_equals;

  EnumFieldAnalyzerState()
      : enum_equals(static_cast<DexMethod*>(DexMethod::get_method(
            "Ljava/lang/Enum;.equals:(Ljava/lang/Object;)Z"))) {}
};

/*
 * EnumFieldAnalyzer::analyze_sget assumes that when it is called to analyze
 * some `sget-object LFoo;.X:LFoo` instruction, the sget instruction is not
 * contained within Foo's class initializer. This means that most users of this
 * analyzer should put it after the ClinitFieldAnalyzer when building the
 * combined analyzer.
 */
class EnumFieldAnalyzer final
    : public InstructionAnalyzerBase<EnumFieldAnalyzer,
                                     ConstantEnvironment,
                                     EnumFieldAnalyzerState> {
 public:
  static bool analyze_sget(const EnumFieldAnalyzerState&,
                           const IRInstruction*,
                           ConstantEnvironment*);
  static bool analyze_invoke(const EnumFieldAnalyzerState&,
                             const IRInstruction*,
                             ConstantEnvironment*);
};

/**
 * Specify how an immutatble field is initialized through function call.
 * For instance, a boxed integer is usually constructed through
 * `Integer.valueOf(I)` invocation with a primitive value, the primitive value
 * can be retrieved through `Integer.intValue()`.
 */
struct ImmutableAttributeAnalyzerState {
  struct Initializer {
    ImmutableAttr::Attr attr;
    size_t insn_src_id_of_attr;
    // The object is passed in as a source register of the method invocation,
    // like `this` pointer of constructor.
    boost::optional<size_t> insn_src_id_of_obj = boost::none;
    // The object is the return value of the method. Example, Integer.valueOf(I)
    bool obj_is_dest() const { return insn_src_id_of_obj == boost::none; }

    explicit Initializer(DexMethod* method_attr) : attr(method_attr) {}
    explicit Initializer(DexField* field_attr) : attr(field_attr) {}

    Initializer& set_src_id_of_attr(size_t id) {
      insn_src_id_of_attr = id;
      return *this;
    }
    Initializer& set_obj_to_dest() {
      insn_src_id_of_obj = boost::none;
      return *this;
    }
    Initializer& set_src_id_of_obj(size_t id) {
      insn_src_id_of_obj = id;
      return *this;
    }
  };

  struct CachedBoxedObjects {
    long begin;
    long end;
  };

  ConcurrentMap<DexMethod*, std::vector<std::unique_ptr<Initializer>>>
      method_initializers;
  ConcurrentSet<DexMethod*> attribute_methods;
  ConcurrentSet<DexField*> attribute_fields;
  std::unordered_map<DexMethod*, CachedBoxedObjects> cached_boxed_objects;

  ImmutableAttributeAnalyzerState();

  // Immutable state should not be updated in parallel with analysis!

  Initializer& add_initializer(DexMethod* initialize_method, DexMethod* attr);
  Initializer& add_initializer(DexMethod* initialize_method, DexField* attr);
  Initializer& add_initializer(DexMethod* initialize_method,
                               const ImmutableAttr::Attr& attr);
  void add_cached_boxed_objects(DexMethod* initialize_method,
                                long begin,
                                long end);
  bool is_jvm_cached_object(DexMethod* initialize_method, long value) const;

  static DexType* initialized_type(const DexMethod* initialize_method);
};

class ImmutableAttributeAnalyzer final
    : public InstructionAnalyzerBase<ImmutableAttributeAnalyzer,
                                     ConstantEnvironment,
                                     ImmutableAttributeAnalyzerState*> {
  static bool analyze_method_initialization(
      const ImmutableAttributeAnalyzerState*,
      const IRInstruction*,
      ConstantEnvironment*,
      DexMethod* method);
  static bool analyze_method_attr(const ImmutableAttributeAnalyzerState*,
                                  const IRInstruction*,
                                  ConstantEnvironment*,
                                  DexMethod* method);

 public:
  static bool analyze_invoke(const ImmutableAttributeAnalyzerState*,
                             const IRInstruction*,
                             ConstantEnvironment*);
  static bool analyze_iget(const ImmutableAttributeAnalyzerState*,
                           const IRInstruction* insn,
                           ConstantEnvironment* env);
};

struct BoxedBooleanAnalyzerState {
  // Construction of this object is expensive because get_type/field/method
  // acquires a global lock. `get` creates a singleton once and reuses it.
  static const BoxedBooleanAnalyzerState& get();

  const DexType* boolean_class{DexType::get_type("Ljava/lang/Boolean;")};
  const DexField* boolean_true{static_cast<DexField*>(
      DexField::get_field("Ljava/lang/Boolean;.TRUE:Ljava/lang/Boolean;"))};
  const DexField* boolean_false{static_cast<DexField*>(
      DexField::get_field("Ljava/lang/Boolean;.FALSE:Ljava/lang/Boolean;"))};
  const DexMethod* boolean_valueof{
      static_cast<DexMethod*>(DexMethod::get_method(
          "Ljava/lang/Boolean;.valueOf:(Z)Ljava/lang/Boolean;"))};
  const DexMethod* boolean_booleanvalue{static_cast<DexMethod*>(
      DexMethod::get_method("Ljava/lang/Boolean;.booleanValue:()Z"))};
};

class BoxedBooleanAnalyzer final
    : public InstructionAnalyzerBase<BoxedBooleanAnalyzer,
                                     ConstantEnvironment,
                                     BoxedBooleanAnalyzerState> {
 public:
  static bool analyze_sget(const BoxedBooleanAnalyzerState&,
                           const IRInstruction*,
                           ConstantEnvironment*);
  static bool analyze_invoke(const BoxedBooleanAnalyzerState&,
                             const IRInstruction*,
                             ConstantEnvironment*);
};

class StringAnalyzer
    : public InstructionAnalyzerBase<StringAnalyzer, ConstantEnvironment> {
 public:
  static bool analyze_const_string(const IRInstruction* insn,
                                   ConstantEnvironment* env) {
    env->set(RESULT_REGISTER, StringDomain(insn->get_string()));
    return true;
  }

  static bool analyze_invoke(const IRInstruction*, ConstantEnvironment*);
};

class ConstantClassObjectAnalyzer
    : public InstructionAnalyzerBase<ConstantClassObjectAnalyzer,
                                     ConstantEnvironment> {
 public:
  static bool analyze_const_class(const IRInstruction* insn,
                                  ConstantEnvironment* env) {
    env->set(RESULT_REGISTER, ConstantClassObjectDomain(insn->get_type()));
    return true;
  }
};

struct ApiLevelAnalyzerState {
  // Construction of this object is expensive because get_method acquires a
  // global lock. `get` caches those lookups.
  static ApiLevelAnalyzerState get(int32_t min_sdk = 0);

  const DexFieldRef* sdk_int_field;
  int32_t min_sdk;
};

class ApiLevelAnalyzer final
    : public InstructionAnalyzerBase<ApiLevelAnalyzer,
                                     ConstantEnvironment,
                                     ApiLevelAnalyzerState> {
 public:
  static bool analyze_sget(const ApiLevelAnalyzerState& state,
                           const IRInstruction* insn,
                           ConstantEnvironment* env);
};

/*
 * Utility methods.
 */

/*
 * runtime_equals_visitor and runtime_leq_visitor are handling different
 * notions of equality / order than AbstractDomain::equals() and
 * AbstractDomain::leq(). The former return true if they can prove that their
 * respective relations hold for a runtime comparison (e.g. from an if-eq or
 * packed-switch instruction). In contrast, AbstractDomain::equals() will
 * return true for two domains representing integers > 0, even though their
 * corresponding runtime values may be different integers.
 */
class runtime_equals_visitor : public boost::static_visitor<bool> {
 public:
  bool operator()(const SignedConstantDomain& scd_left,
                  const SignedConstantDomain& scd_right) const {
    auto cst_left = scd_left.get_constant();
    auto cst_right = scd_right.get_constant();
    if (!(cst_left && cst_right)) {
      return false;
    }
    return *cst_left == *cst_right;
  }

  // SingletonObjectDomain and StringDomains are equal iff their respective
  // constants are equal.
  template <typename Constant,
            typename = typename std::enable_if_t<
                template_util::contains<Constant,
                                        const DexField*,
                                        const DexString*,
                                        const DexType*>::value>>
  bool operator()(const sparta::ConstantAbstractDomain<Constant>& d1,
                  const sparta::ConstantAbstractDomain<Constant>& d2) const {
    if (!(d1.is_value() && d2.is_value())) {
      return false;
    }
    return *d1.get_constant() == *d2.get_constant();
  }

  bool operator()(const ObjectWithImmutAttrDomain& d1,
                  const ObjectWithImmutAttrDomain& d2) const {
    if (!(d1.is_value() && d2.is_value())) {
      return false;
    }
    return d1.get_constant()->runtime_equals(*d2.get_constant()) ==
           TriState::True;
  }

  template <typename Domain, typename OtherDomain>
  bool operator()(const Domain& d1, const OtherDomain& d2) const {
    return false;
  }
};

class runtime_leq_visitor : public boost::static_visitor<bool> {
 public:
  bool operator()(const SignedConstantDomain& scd_left,
                  const SignedConstantDomain& scd_right) const {
    return scd_left.max_element() <= scd_right.min_element();
  }

  template <typename Domain, typename OtherDomain>
  bool operator()(const Domain& d1, const OtherDomain& d2) const {
    return false;
  }
};

/*
 * Note: We cannot replace the runtime_lt_visitor by combining the
 * runtime_leq_visitor and the negation of the runtime_equals_visitor. Suppose
 * the runtime_leq_visitor returns true and the runtime_equals_visitor returns
 * false. That means that the LHS must be less than or equal to the RHS, and
 * that they *might* not be equal. Since they may still be equal, we cannot
 * conclude that the LHS must be less than the RHS.
 */
class runtime_lt_visitor : public boost::static_visitor<bool> {
 public:
  bool operator()(const SignedConstantDomain& scd_left,
                  const SignedConstantDomain& scd_right) const {
    return scd_left.max_element() < scd_right.min_element();
  }

  template <typename Domain, typename OtherDomain>
  bool operator()(const Domain& d1, const OtherDomain& d2) const {
    return false;
  }
};

/*
 * Analyze the invoke instruction given by :insn by running an abstract
 * interpreter on the :callee_code with the provided arguments. By analyzing the
 * callee with the exact arguments at this callsite -- instead of the join of
 * the arguments at every possible callsites -- we can get more precision in the
 * return value / final heap state of the invoke.
 *
 * :env should represent the ConstantEnvironment at the point before the invoke
 * instruction gets executed. semantically_inline_method will update :env to
 * reflect the ConstantEnvironment at the point after the invoke instruction
 * has finished executing.
 *
 * Note that this method will not recursively inline invocations within
 * :callee_code. We can extend it to support that in the future.
 */
void semantically_inline_method(
    IRCode* callee_code,
    const IRInstruction* insn,
    const InstructionAnalyzer<ConstantEnvironment>& analyzer,
    ConstantEnvironment* env);

/*
 * Do a join over the states at each return opcode in a method.
 */
ReturnState collect_return_state(IRCode* code,
                                 const intraprocedural::FixpointIterator&);

/*
 * Get source argument index, if any, which is getting dereferenced by an
 * instruction. A null object would cause an exception to be thrown.
 */
boost::optional<size_t> get_dereferenced_object_src_index(const IRInstruction*);

/*
 * Handle reading of static fields fXXX in $EnumUtils class.
 */
class EnumUtilsFieldAnalyzer final
    : public InstructionAnalyzerBase<EnumUtilsFieldAnalyzer,
                                     ConstantEnvironment,
                                     ImmutableAttributeAnalyzerState*> {
 public:
  static bool analyze_sget(ImmutableAttributeAnalyzerState* state,
                           const IRInstruction* insn,
                           ConstantEnvironment* env);
};

using ConstantPrimitiveAndBoxedAnalyzer =
    InstructionAnalyzerCombiner<EnumUtilsFieldAnalyzer,
                                ImmutableAttributeAnalyzer,
                                EnumFieldAnalyzer,
                                BoxedBooleanAnalyzer,
                                StringAnalyzer,
                                ApiLevelAnalyzer,
                                PrimitiveAnalyzer>;

} // namespace constant_propagation
