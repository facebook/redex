/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FieldOpTracker.h"

#include "BaseIRAnalyzer.h"
#include "ConcurrentContainers.h"
#include "ConstantAbstractDomain.h"
#include "ControlFlow.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "Resolver.h"
#include "Walkers.h"

namespace field_op_tracker {

bool is_own_init(DexField* field, const DexMethod* method) {
  return (method::is_clinit(method) || method::is_init(method)) &&
         method->get_class() == field->get_class();
}

using namespace ir_analyzer;

using IsZeroDomain = sparta::ConstantAbstractDomain<bool>;
using IsZeroEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<reg_t, IsZeroDomain>;

class IsZeroAnalyzer final : public BaseIRAnalyzer<IsZeroEnvironment> {

 public:
  IsZeroAnalyzer(const cfg::ControlFlowGraph& cfg,
                 NonZeroWrittenFields& non_zero_written_fields)
      : BaseIRAnalyzer(cfg),
        m_non_zero_written_fields(non_zero_written_fields) {
    MonotonicFixpointIterator::run(IsZeroEnvironment::top());
  }

  void analyze_instruction(const IRInstruction* insn,
                           IsZeroEnvironment* current_state) const override {

    auto op = insn->opcode();
    if (is_iput(op) || is_sput(op)) {
      const auto value = current_state->get(insn->src(0));
      // reachable?
      if (!value.is_bottom()) {
        // could be non-zero?
        if (value.is_top() || !*value.get_constant()) {
          auto field = resolve_field(insn->get_field());
          // we can resolve field?
          if (field != nullptr) {
            m_non_zero_written_fields.insert(field);
          }
        }
      }
    } else if (op == OPCODE_CONST || op == OPCODE_CONST_WIDE) {
      current_state->set(insn->dest(), IsZeroDomain(insn->get_literal() == 0));
    } else if (insn->has_dest()) {
      current_state->set(insn->dest(), IsZeroDomain::top());
    }
  }

 private:
  NonZeroWrittenFields& m_non_zero_written_fields;
};

NonZeroWrittenFields analyze_non_zero_writes(const Scope& scope) {
  ConcurrentSet<DexField*> concurrent_non_zero_written_fields;
  // Gather the read/write counts.
  walk::parallel::code(scope, [&](const DexMethod*, const IRCode& code) {
    NonZeroWrittenFields non_zero_written_fields;
    IsZeroAnalyzer analyzer(code.cfg(), non_zero_written_fields);
    for (auto f : non_zero_written_fields) {
      concurrent_non_zero_written_fields.insert(f);
    }
  });
  return NonZeroWrittenFields(concurrent_non_zero_written_fields.begin(),
                              concurrent_non_zero_written_fields.end());
}

FieldStatsMap analyze(const Scope& scope) {
  FieldStatsMap field_stats;
  // Gather the read/write counts.
  walk::opcodes(scope, [&](const DexMethod* method, const IRInstruction* insn) {
    auto op = insn->opcode();
    if (!insn->has_field()) {
      return;
    }
    auto field = resolve_field(insn->get_field());
    if (field == nullptr) {
      return;
    }
    if (is_sget(op) || is_iget(op)) {
      ++field_stats[field].reads;
      if (!is_own_init(field, method)) {
        ++field_stats[field].reads_outside_init;
      }
    } else if (is_sput(op) || is_iput(op)) {
      ++field_stats[field].writes;
    }
  });
  return field_stats;
}

} // namespace field_op_tracker
