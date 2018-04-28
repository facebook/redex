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
  struct Config {
    // If we are analyzing a class initializer, this is expected to point to
    // the DexType of the class. It indicates that the analysis can treat the
    // static fields of this class as non-escaping.
    DexType* class_under_init{nullptr};
  };

  /*
   * The fixpoint iterator takes an optional WholeProgramState argument that
   * it will use to determine the static field values and method return values.
   */
  explicit FixpointIterator(
      const cfg::ControlFlowGraph& cfg,
      const std::function<void(const IRInstruction*, ConstantEnvironment*)>
          insn_analyzer)
      : MonotonicFixpointIterator(cfg), m_insn_analyzer(insn_analyzer) {}

  ConstantEnvironment analyze_edge(
      const std::shared_ptr<cfg::Edge>&,
      const ConstantEnvironment& exit_state_at_source) const override;

  void analyze_instruction(const IRInstruction* insn,
                           ConstantEnvironment* current_state) const;

  void analyze_node(const NodeId& block,
                    ConstantEnvironment* state_at_entry) const override;

 private:
  std::function<void(const IRInstruction*, ConstantEnvironment*)>
      m_insn_analyzer;
};

} // namespace intraprocedural

/*
 * Propagates primitive constants and handles simple arithmetic. Also defines
 * an analyze_default implementation that simply sets any modified registers
 * to Top. This sub-analyzer should typically be the last one in any list of
 * combined sub-analyzers.
 */
class ConstantPrimitiveSubAnalyzer final
    : public InstructionSubAnalyzerBase<ConstantPrimitiveSubAnalyzer,
                                        ConstantEnvironment> {
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
    InstructionSubAnalyzerCombiner<ConstantPrimitiveSubAnalyzer>;

/*
 * Handle non-escaping arrays.
 */
class LocalArraySubAnalyzer final
    : public InstructionSubAnalyzerBase<LocalArraySubAnalyzer,
                                        ConstantEnvironment> {
 public:
  static bool analyze_new_array(const IRInstruction* insn,
                                ConstantEnvironment* env);

  static bool analyze_aget(const IRInstruction* insn, ConstantEnvironment* env);

  static bool analyze_aput(const IRInstruction* insn, ConstantEnvironment* env);

  static bool analyze_sput(const IRInstruction* insn, ConstantEnvironment* env);

  static bool analyze_iput(const IRInstruction* insn, ConstantEnvironment* env);

  static bool analyze_fill_array_data(const IRInstruction* insn,
                                      ConstantEnvironment* env);

  static bool analyze_invoke(const IRInstruction* insn,
                             ConstantEnvironment* env);

 private:
  static void mark_array_unknown(reg_t reg, ConstantEnvironment* env);
};

/*
 * Handle static fields in <clinit> methods. Since class initializers must (in
 * most cases) complete running before any other piece of code can modify these
 * fields, we can treat them as non-escaping while analyzing these methods.
 */
class ClinitFieldSubAnalyzer final
    : public InstructionSubAnalyzerBase<ClinitFieldSubAnalyzer,
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

struct EnumFieldSubAnalyzerState {
  const DexMethod* enum_equals;
  EnumFieldSubAnalyzerState()
      : enum_equals(static_cast<DexMethod*>(DexMethod::get_method(
            "Ljava/lang/Enum;.equals:(Ljava/lang/Object;)Z"))) {
    always_assert(enum_equals);
  }
};

/*
 * EnumFieldSubAnalyzer::analyze_sget assumes that when it is called to analyze
 * some `sget-object LFoo;.X:LFoo` instruction, the sget instruction is not
 * contained within Foo's class initializer. This means that most users of this
 * analyzer should put it after the ClinitFieldSubAnalyzer when building the
 * combined analyzer.
 */
class EnumFieldSubAnalyzer final
    : public InstructionSubAnalyzerBase<EnumFieldSubAnalyzer,
                                        ConstantEnvironment,
                                        EnumFieldSubAnalyzerState> {
 public:
  static bool analyze_sget(const EnumFieldSubAnalyzerState&,
                           const IRInstruction*,
                           ConstantEnvironment*);
  static bool analyze_invoke(const EnumFieldSubAnalyzerState&,
                             const IRInstruction*,
                             ConstantEnvironment*);
};

struct BoxedBooleanSubAnalyzerState {
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

class BoxedBooleanSubAnalyzer final
    : public InstructionSubAnalyzerBase<BoxedBooleanSubAnalyzer,
                                        ConstantEnvironment,
                                        BoxedBooleanSubAnalyzerState> {
 public:
  static bool analyze_sget(const BoxedBooleanSubAnalyzerState&,
                           const IRInstruction*,
                           ConstantEnvironment*);
  static bool analyze_invoke(const BoxedBooleanSubAnalyzerState&,
                             const IRInstruction*,
                             ConstantEnvironment*);
};

} // namespace constant_propagation
