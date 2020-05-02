/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
  LocalTypeAnalyzer(
      const cfg::ControlFlowGraph& cfg,
      InstructionAnalyzer<DexTypeEnvironment> insn_analyer,
      std::unique_ptr<std::unordered_set<DexField*>> written_fields)
      : ir_analyzer::BaseIRAnalyzer<DexTypeEnvironment>(cfg),
        m_insn_analyzer(std::move(insn_analyer)) {
    m_written_fields = std::move(written_fields);
  }

  void analyze_instruction(const IRInstruction* insn,
                           DexTypeEnvironment* current_state) const override;

 private:
  InstructionAnalyzer<DexTypeEnvironment> m_insn_analyzer;
  /*
   * Tracking the fields referenced by the method that have been written to.
   * It shares the same life time with the LocalTypeAnalyzer.
   */
  std::unique_ptr<std::unordered_set<DexField*>> m_written_fields;
};

class RegisterTypeAnalyzer final
    : public InstructionAnalyzerBase<RegisterTypeAnalyzer, DexTypeEnvironment> {
 public:
  static bool analyze_default(const IRInstruction* insn,
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
};

class FieldTypeAnalyzer final
    : public InstructionAnalyzerBase<
          FieldTypeAnalyzer,
          DexTypeEnvironment,
          std::unordered_set<DexField*>* /* written fields */> {
 public:
  static bool analyze_iget(std::unordered_set<DexField*>* written_fields,
                           const IRInstruction* insn,
                           DexTypeEnvironment* env);
  static bool analyze_iput(std::unordered_set<DexField*>* written_fields,
                           const IRInstruction* insn,
                           DexTypeEnvironment* env);
  static bool analyze_sget(std::unordered_set<DexField*>* written_fields,
                           const IRInstruction* insn,
                           DexTypeEnvironment* env);
  static bool analyze_sput(std::unordered_set<DexField*>* written_fields,
                           const IRInstruction* insn,
                           DexTypeEnvironment* env);
};

} // namespace local

} // namespace type_analyzer
