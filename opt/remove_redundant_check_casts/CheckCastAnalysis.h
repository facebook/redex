/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <vector>

#include "BaseIRAnalyzer.h"
#include "ConstantAbstractDomain.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "IRCode.h"
#include "PatriciaTreeMapAbstractEnvironment.h"

namespace check_casts {

namespace impl {

using register_t = ir_analyzer::register_t;

using Domain = sparta::ConstantAbstractDomain<const DexType*>;

using Environment =
    sparta::PatriciaTreeMapAbstractEnvironment<register_t, Domain>;

class CheckCastAnalysis final
    : public ir_analyzer::BaseIRAnalyzer<Environment> {
 public:
  explicit CheckCastAnalysis(cfg::ControlFlowGraph* cfg, DexMethod* method)
      : ir_analyzer::BaseIRAnalyzer<Environment>(*cfg), m_method(method) {}

  std::unordered_map<IRInstruction*, boost::optional<IRInstruction*>>
  collect_redundant_checks_replacement();

 private:
  void analyze_instruction(IRInstruction* insn,
                           Environment* env) const override;
  const DexMethod* m_method;
};

} // namespace impl

} // namespace check_casts
