/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "ConstantEnvironment.h"
#include "IRCode.h"
#include "InstructionAnalyzer.h"

namespace constant_propagation {

namespace intraprocedural {

class FixpointIterator final
    : public MonotonicFixpointIterator<cfg::GraphInterface,
                                       ConstantEnvironment> {
 public:
  /*
   * The fixpoint iterator takes an optional WholeProgramState argument that
   * it will use to determine the static field values and method return values.
   */
  explicit FixpointIterator(
      const cfg::ControlFlowGraph& cfg,
      InstructionAnalyzer<ConstantEnvironment> insn_analyzer)
      : MonotonicFixpointIterator(cfg), m_insn_analyzer(insn_analyzer) {}

  ConstantEnvironment analyze_edge(
      const EdgeId&,
      const ConstantEnvironment& exit_state_at_source) const override;

  void analyze_instruction(const IRInstruction* insn,
                           ConstantEnvironment* current_state) const;

  void analyze_node(const NodeId& block,
                    ConstantEnvironment* state_at_entry) const override;

 private:
  InstructionAnalyzer<ConstantEnvironment> m_insn_analyzer;
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
};

/*
 * Handle non-escaping arrays.
 *
 * This Analyzer should typically be used followed by the
 * HeapEscapeAnalyzer in a combined analysis -- LocalArrayAnalyzer only
 * handles the creation and mutation of array values, but does not account for
 * how they may escape.
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
 * Handle static fields in <clinit> methods. Since class initializers must (in
 * most cases) complete running before any other piece of code can modify these
 * fields, we can treat them as non-escaping while analyzing these methods.
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

struct EnumFieldAnalyzerState {
  const DexMethod* enum_equals;
  EnumFieldAnalyzerState()
      : enum_equals(static_cast<DexMethod*>(DexMethod::get_method(
            "Ljava/lang/Enum;.equals:(Ljava/lang/Object;)Z"))) {
    always_assert(enum_equals);
  }
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

struct BoxedBooleanAnalyzerState {
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

/*
 * Utility methods.
 */

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

} // namespace constant_propagation
