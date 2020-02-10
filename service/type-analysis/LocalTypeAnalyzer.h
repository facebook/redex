/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional/optional_io.hpp>

#include "BaseIRAnalyzer.h"
#include "DexTypeDomain.h"
#include "InstructionAnalyzer.h"
#include "PatriciaTreeMapAbstractEnvironment.h"

namespace type_analyzer {

namespace local {

using namespace ir_analyzer;

using DexTypeEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<reg_t, DexTypeDomain>;

class LocalTypeAnalyzer final
    : public ir_analyzer::BaseIRAnalyzer<DexTypeEnvironment> {
 public:
  LocalTypeAnalyzer(const cfg::ControlFlowGraph& cfg,
                    InstructionAnalyzer<DexTypeEnvironment> insn_analyer)
      : ir_analyzer::BaseIRAnalyzer<DexTypeEnvironment>(cfg),
        m_insn_analyzer(std::move(insn_analyer)) {}

  void analyze_instruction(const IRInstruction* insn,
                           DexTypeEnvironment* current_state) const override;

 private:
  InstructionAnalyzer<DexTypeEnvironment> m_insn_analyzer;
};

class InstructionTypeAnalyzer final
    : public InstructionAnalyzerBase<InstructionTypeAnalyzer,
                                     DexTypeEnvironment> {
 public:
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

} // namespace local

} // namespace type_analyzer
