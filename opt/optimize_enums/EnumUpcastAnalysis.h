/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <limits>

#include "BaseIRAnalyzer.h"
#include "ConcurrentContainers.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "PatriciaTreeSetAbstractDomain.h"

namespace optimize_enums {

using Register = ir_analyzer::register_t;

// Store possible types for a register although we only care about Object, Enum
// and Enum's subtypes.
using EnumTypes = sparta::PatriciaTreeSetAbstractDomain<DexType*>;
using EnumTypeEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<Register, EnumTypes>;

class EnumFixpointIterator final
    : public ir_analyzer::BaseIRAnalyzer<EnumTypeEnvironment> {
 public:
  explicit EnumFixpointIterator(const cfg::ControlFlowGraph& cfg)
      : ir_analyzer::BaseIRAnalyzer<EnumTypeEnvironment>(cfg) {}

  void analyze_instruction(IRInstruction* insn,
                           EnumTypeEnvironment* env) const override;

  static EnumTypeEnvironment gen_env(const DexMethod* method);
};

void reject_unsafe_enums(const std::vector<DexClass*>& classes,
                         ConcurrentSet<DexType*>* candidate_enums);
} // namespace optimize_enums
