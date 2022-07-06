/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OptimizeEnumsAnalysis.h"

#include "BaseIRAnalyzer.h"
#include "ConstantEnvironment.h"
#include "IRCode.h"
#include "Resolver.h"

using namespace sparta;

namespace {

void analyze_move(const IRInstruction* insn, ConstantEnvironment* env) {
  always_assert(opcode::is_a_move(insn->opcode()));

  auto src = insn->src(0);
  auto dst = insn->dest();

  auto cst = env->get<SignedConstantDomain>(src).get_constant();
  if (!cst) {
    env->set(dst, ConstantValue::top());
  } else {
    env->set(dst, SignedConstantDomain(*cst));
  }
}

} // namespace

namespace optimize_enums {

namespace impl {

class Analyzer final : public ir_analyzer::BaseIRAnalyzer<ConstantEnvironment> {

 public:
  Analyzer(
      const cfg::ControlFlowGraph& cfg,
      const std::unordered_map<const DexMethod*, uint32_t>& ctor_to_arg_ordinal,
      const DexClass* cls)
      : ir_analyzer::BaseIRAnalyzer<ConstantEnvironment>(cfg),
        m_ctor_to_arg_ordinal(ctor_to_arg_ordinal),
        m_current_enum(cls) {
    MonotonicFixpointIterator::run(ConstantEnvironment::top());
  }

  void analyze_instruction(const IRInstruction* insn,
                           ConstantEnvironment* env) const override {
    auto op = insn->opcode();

    auto default_case = [&]() {
      if (insn->has_dest()) {
        env->set(insn->dest(), ConstantValue::top());
        if (insn->dest_is_wide()) {
          env->set(insn->dest() + 1, ConstantValue::top());
        }
      } else if (insn->has_move_result_any()) {
        env->set(RESULT_REGISTER, ConstantValue::top());
      }
    };

    switch (op) {
    case IOPCODE_LOAD_PARAM:
    case IOPCODE_LOAD_PARAM_WIDE:
    case IOPCODE_LOAD_PARAM_OBJECT: {
      not_reached_log("<clinit> is static and doesn't take any arguments");
    }

    case OPCODE_CONST:
    case OPCODE_CONST_WIDE: {
      // Keep track of the actual ordinals.
      env->set(insn->dest(), SignedConstantDomain(insn->get_literal()));
      break;
    }

    case OPCODE_MOVE:
    case OPCODE_MOVE_OBJECT:
    case OPCODE_MOVE_WIDE: {
      analyze_move(insn, env);
      break;
    }

    case OPCODE_SPUT_OBJECT: {
      auto field = resolve_field(insn->get_field(), FieldSearch::Static);
      if (!field) {
        default_case();
        break;
      }

      // Get the ordinal associated with the register that holds the instance.
      if (field->get_type() == m_current_enum->get_type()) {
        env->set(field, env->get<SignedConstantDomain>(insn->src(0)));
      }
      break;
    }

    case OPCODE_INVOKE_DIRECT: {
      auto invoked = resolve_method(insn->get_method(), MethodSearch::Direct);
      if (!invoked) {
        default_case();
        break;
      }

      if (method::is_init(invoked) &&
          invoked->get_class() == m_current_enum->get_type()) {
        // We keep track of the ordinal value of the newly created instance
        // in the register that holds the instance.
        //
        // For example:
        //
        //  CONST <v_ordinal>, <literal> // Here we set <v_ordinal> to hold
        //                               // the literal
        //  ...
        //  INVOKE_DIRECT <v_enum>, ..., <v_ordinal>, ...  // Here we set
        //    // <v_enum> to hold the literal.
        auto arg = m_ctor_to_arg_ordinal.at(invoked);
        auto src = insn->src(arg);
        auto cst = env->get<SignedConstantDomain>(src).get_constant();

        always_assert(cst);
        env->set(insn->src(0), SignedConstantDomain(*cst));
      }
      break;
    }

    default: {
      default_case();
      break;
    }
    }
  }

 private:
  const std::unordered_map<const DexMethod*, uint32_t> m_ctor_to_arg_ordinal;
  const DexClass* m_current_enum;
};

} // namespace impl

OptimizeEnumsAnalysis::~OptimizeEnumsAnalysis() {}

OptimizeEnumsAnalysis::OptimizeEnumsAnalysis(
    const DexClass* enum_cls,
    const std::unordered_map<const DexMethod*, uint32_t>& ctor_to_arg_ordinal)
    : m_cls(enum_cls) {
  auto clinit = enum_cls->get_clinit();
  always_assert(clinit && clinit->get_code());

  m_clinit_cfg = cfg::ScopedCFG(clinit->get_code());
  m_clinit_cfg->calculate_exit_block();
  m_analyzer = std::make_unique<impl::Analyzer>(
      *m_clinit_cfg, ctor_to_arg_ordinal, enum_cls);
}

/**
 * Collect ordinals for all the enum fields, if all of them can be
 * statically determined. Otherwise, none.
 */
void OptimizeEnumsAnalysis::collect_ordinals(
    std::unordered_map<DexField*, size_t>& enum_field_to_ordinal) {
  auto env = m_analyzer->get_exit_state_at(m_clinit_cfg->exit_block());

  bool are_all_ordinals_determined = true;
  for (const auto& sfield : m_cls->get_sfields()) {
    if (sfield->get_type() == m_cls->get_type()) {
      auto cst = env.get<SignedConstantDomain>(sfield).get_constant();
      if (!cst) {
        are_all_ordinals_determined = false;
        break;
      }

      enum_field_to_ordinal[sfield] = *cst;
    }
  }

  // If not all ordinals were properly determined, cleanup.
  if (!are_all_ordinals_determined) {
    for (const auto& sfield : m_cls->get_sfields()) {
      enum_field_to_ordinal.erase(sfield);
    }
  }
}

} // namespace optimize_enums
