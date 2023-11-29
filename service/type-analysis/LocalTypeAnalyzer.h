/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional/optional_io.hpp>

#include "BaseIRAnalyzer.h"
#include "DexTypeEnvironment.h"
#include "InstructionAnalyzer.h"

namespace type_analyzer {

namespace local {

using namespace ir_analyzer;

class LocalTypeAnalyzer final
    : public ir_analyzer::BaseIRAnalyzer<DexTypeEnvironment> {
 public:
  LocalTypeAnalyzer(const cfg::ControlFlowGraph& cfg,
                    InstructionAnalyzer<DexTypeEnvironment> insn_analyer)
      : ir_analyzer::BaseIRAnalyzer<DexTypeEnvironment>(cfg),
        m_insn_analyzer(std::move(insn_analyer)) {}

  void analyze_instruction(const IRInstruction* insn,
                           DexTypeEnvironment* env) const override;

 private:
  InstructionAnalyzer<DexTypeEnvironment> m_insn_analyzer;
};

class RegisterTypeAnalyzer final
    : public InstructionAnalyzerBase<RegisterTypeAnalyzer, DexTypeEnvironment> {
 public:
  static bool analyze_default(const IRInstruction* insn,
                              DexTypeEnvironment* env);

  static bool analyze_check_cast(const IRInstruction* insn,
                                 DexTypeEnvironment* env);

  static bool analyze_const(const IRInstruction* insn, DexTypeEnvironment* env);

  static bool analyze_const_string(const IRInstruction*,
                                   DexTypeEnvironment* env);

  static bool analyze_const_class(const IRInstruction*,
                                  DexTypeEnvironment* env);

  static bool analyze_aget(const IRInstruction* insn, DexTypeEnvironment* env);

  static bool analyze_aput(const IRInstruction* insn, DexTypeEnvironment* env);

  static bool analyze_array_length(const IRInstruction* insn,
                                   DexTypeEnvironment* env);

  static bool analyze_binop_lit(const IRInstruction* insn,
                                DexTypeEnvironment* env);

  static bool analyze_binop(const IRInstruction* insn, DexTypeEnvironment* env);

  static bool analyze_move(const IRInstruction* insn, DexTypeEnvironment* env);

  static bool analyze_move_result(const IRInstruction* insn,
                                  DexTypeEnvironment* env);

  static bool analyze_move_exception(const IRInstruction* insn,
                                     DexTypeEnvironment* env);

  static bool analyze_new_instance(const IRInstruction* insn,
                                   DexTypeEnvironment* env);

  static bool analyze_new_array(const IRInstruction* insn,
                                DexTypeEnvironment* env);

  static bool analyze_filled_new_array(const IRInstruction* insn,
                                       DexTypeEnvironment* env);

  static bool analyze_invoke(const IRInstruction* insn,
                             DexTypeEnvironment* env);
};

/*
 * Unlike in other methods where we always propagate field type info from
 * the WholeProgramState, in <clinit>s, we directly propagate static field type
 * info through the local FieldTypeEnvironment. This is similar to what we do
 * for constant values in IPCP.
 *
 * The reason is that the <clinit> is the 1st method of the class being executed
 * after class loading. Therefore, the field 'writes' in the <clinit> happens
 * before other 'writes' to the same field. In other words, the field type state
 * of <clinit>s are self-contained. Note that we are limiting ourselves to
 * static fields belonging to the same class here.
 *
 * We don't throw away our results if there're invoke-statics in the <clinit>.
 * Since the field 'write' in the invoke-static callee will be aggregated in the
 * final type mapping in WholeProgramState. Before that happens, we do not
 * propagate incomplete field type info to other methods. As stated above, we
 * only propagate field type info from WholeProgramState computed in the
 * previous global iteration.
 */
class ClinitFieldAnalyzer final
    : public InstructionAnalyzerBase<ClinitFieldAnalyzer,
                                     DexTypeEnvironment,
                                     DexType* /* class_under_init */> {
 public:
  static bool analyze_sget(const DexType* class_under_init,
                           const IRInstruction* insn,
                           DexTypeEnvironment* env);

  static bool analyze_sput(const DexType* class_under_init,
                           const IRInstruction* insn,
                           DexTypeEnvironment* env);
};

/*
 * Similarly CtorFieldAnalyzer populates local FieldTypeEnvironment when
 * analyzing a ctor. We only do so for instance fields that belong to the class
 * the ctor is under. When collecting the WholeProgramState, we first collect
 * the end state of the FieldTypeEnvironment for all ctors. We use that as the
 * initial type mapping for all instance fields.
 *
 * Note that we only update Field type mapping for operations on `this` obj. We
 * do not want to collect field type update on another instance of the same
 * class. That's not correct. As a result, we might incorrectly initialize the
 * nullness of a field without the instance tracking.
 */
class CtorFieldAnalyzer final
    : public InstructionAnalyzerBase<CtorFieldAnalyzer,
                                     DexTypeEnvironment,
                                     DexType* /* class_under_init */> {
 public:
  static bool analyze_default(const DexType* class_under_init,
                              const IRInstruction* insn,
                              DexTypeEnvironment* env);
  static bool analyze_load_param(const DexType* class_under_init,
                                 const IRInstruction* insn,
                                 DexTypeEnvironment* env);
  static bool analyze_iget(const DexType* class_under_init,
                           const IRInstruction* insn,
                           DexTypeEnvironment* env);

  static bool analyze_iput(const DexType* class_under_init,
                           const IRInstruction* insn,
                           DexTypeEnvironment* env);

  static bool analyze_move(const DexType* class_under_init,
                           const IRInstruction* insn,
                           DexTypeEnvironment* env);

  static bool analyze_move_result(const DexType* class_under_init,
                                  const IRInstruction* insn,
                                  DexTypeEnvironment* env);

  static bool analyze_invoke(const DexType* class_under_init,
                             const IRInstruction* insn,
                             DexTypeEnvironment* env);
};

} // namespace local

} // namespace type_analyzer
