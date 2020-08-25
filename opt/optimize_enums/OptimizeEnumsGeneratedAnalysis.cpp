/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OptimizeEnumsGeneratedAnalysis.h"

#include "BaseIRAnalyzer.h"
#include "ConstantEnvironment.h"
#include "IRCode.h"
#include "Resolver.h"

using namespace sparta;

namespace optimize_enums {

namespace impl {

using DexFieldConstantDomain = ConstantAbstractDomain<DexField*>;

/**
 * For each register keep track of the field it holds.
 */
using DexFieldConstantEnvironment =
    PatriciaTreeMapAbstractEnvironment<reg_t, DexFieldConstantDomain>;

namespace {

constexpr const char* ENUM_TYPE = "Ljava/lang/Enum;";
constexpr const char* ORDINAL_METHOD_NAME = "ordinal";

void analyze_move_fields(const IRInstruction* insn,
                         DexFieldConstantEnvironment* env) {
  always_assert(opcode::is_a_move(insn->opcode()));

  auto src = insn->src(0);
  auto dst = insn->dest();

  auto cst = env->get(src).get_constant();
  if (!cst) {
    env->set(dst, DexFieldConstantDomain::top());
  } else {
    env->set(dst, DexFieldConstantDomain(*cst));
  }
}

} // namespace

class FieldAnalyzer final
    : public ir_analyzer::BaseIRAnalyzer<DexFieldConstantEnvironment> {

 public:
  FieldAnalyzer(const cfg::ControlFlowGraph& cfg,
                const DexClass* generated_cls,
                const DexType* current_enum)
      : ir_analyzer::BaseIRAnalyzer<DexFieldConstantEnvironment>(cfg),
        m_generated_cls(generated_cls) {
    MonotonicFixpointIterator::run(DexFieldConstantEnvironment::top());
    setup_ordinal_method();
  }

  void analyze_instruction(const IRInstruction* insn,
                           DexFieldConstantEnvironment* env) const override {
    auto op = insn->opcode();

    auto default_case = [&]() {
      if (insn->has_dest()) {
        env->set(insn->dest(), DexFieldConstantDomain::top());
        if (insn->dest_is_wide()) {
          env->set(insn->dest() + 1, DexFieldConstantDomain::top());
        }
      } else if (insn->has_move_result_any()) {
        env->set(RESULT_REGISTER, DexFieldConstantDomain::top());
      }
    };

    switch (op) {
    case IOPCODE_LOAD_PARAM:
    case IOPCODE_LOAD_PARAM_WIDE:
    case IOPCODE_LOAD_PARAM_OBJECT: {
      not_reached_log("<clinit> is static and doesn't take any arguments");
    }

    case OPCODE_MOVE:
    case OPCODE_MOVE_OBJECT:
    case OPCODE_MOVE_WIDE: {
      analyze_move_fields(insn, env);
      break;
    }

    case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
    case OPCODE_MOVE_RESULT_OBJECT:
    case OPCODE_MOVE_RESULT: {
      env->set(insn->dest(), env->get(RESULT_REGISTER));
      break;
    }

    case OPCODE_SPUT_OBJECT: {
      auto field = resolve_field(insn->get_field(), FieldSearch::Static);
      if (!field) {
        default_case();
        break;
      }

      if (field->get_class() == m_generated_cls->get_type() || is_enum(field)) {
        env->set(insn->src(0), DexFieldConstantDomain(field));
      }
      break;
    }
    case OPCODE_SGET_OBJECT: {
      auto field = resolve_field(insn->get_field(), FieldSearch::Static);
      if (!field) {
        default_case();
        break;
      }

      if (field->get_class() == m_generated_cls->get_type() || is_enum(field)) {
        env->set(RESULT_REGISTER, DexFieldConstantDomain(field));
      }
      break;
    }

    case OPCODE_INVOKE_VIRTUAL: {
      auto invoked = resolve_method(insn->get_method(), MethodSearch::Virtual);
      if (!invoked) {
        default_case();
        break;
      }

      if (invoked == m_ordinal_method) {
        // We keep track of the field the ordinal() method was called on.
        //
        // For example:
        //
        //  SGET <v_enum>, Field // Here we set RESULT_REGISTER to hold
        //                       // the field
        //  MOVE_PSEUDO_RESULT <v_field> // Here we set <v_field> to hold
        //                               // the field
        //  ...
        //  // Here we set RESULT_REGISTER to hold the field.
        //  INVOKE_VIRTUAL <v_field> Enum.ordinal()
        //  MOVE_RESULT <v_ordinal> // Here we set <v_ordinal> to hold the field
        auto cst = env->get(insn->src(0)).get_constant();
        always_assert(cst);

        env->set(RESULT_REGISTER, DexFieldConstantDomain(*cst));
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
  void setup_ordinal_method() {
    auto enum_type = DexType::get_type(ENUM_TYPE);
    auto ordinal_str = DexString::get_string(ORDINAL_METHOD_NAME);
    auto proto =
        DexProto::get_proto(type::_int(), DexTypeList::make_type_list({}));

    auto method_ref = DexMethod::get_method(enum_type, ordinal_str, proto);
    m_ordinal_method = resolve_method(method_ref, MethodSearch::Virtual);
    always_assert(m_ordinal_method);
  }

  const DexClass* m_generated_cls;
  DexMethod* m_ordinal_method;
};

using UInt32ConstantDomain = ConstantAbstractDomain<uint32_t>;

/**
 * For each register keep track of the literal it holds.
 */
using UInt32ConstantEnvironment =
    PatriciaTreeMapAbstractEnvironment<reg_t, UInt32ConstantDomain>;

namespace {

void analyze_move_const(const IRInstruction* insn,
                        UInt32ConstantEnvironment* env) {
  always_assert(opcode::is_a_move(insn->opcode()));

  auto src = insn->src(0);
  auto dst = insn->dest();

  auto cst = env->get(src).get_constant();
  if (!cst) {
    env->set(dst, UInt32ConstantDomain::top());
  } else {
    env->set(dst, UInt32ConstantDomain(*cst));
  }
}

} // namespace

class ConstAnalyzer final
    : public MonotonicFixpointIterator<cfg::GraphInterface,
                                       UInt32ConstantEnvironment> {

 public:
  explicit ConstAnalyzer(const cfg::ControlFlowGraph& cfg)
      : MonotonicFixpointIterator(cfg) {
    MonotonicFixpointIterator::run(UInt32ConstantEnvironment::top());
  }

  void analyze_node(const NodeId& block,
                    UInt32ConstantEnvironment* state_at_entry) const override {
    for (auto& mie : InstructionIterable(block)) {
      analyze_instruction(mie.insn, state_at_entry);
    }
  }

  UInt32ConstantEnvironment analyze_edge(
      const EdgeId&,
      const UInt32ConstantEnvironment& exit_state_at_source) const override {
    return exit_state_at_source;
  }

  void analyze_instruction(const IRInstruction* insn,
                           UInt32ConstantEnvironment* env) const {
    auto op = insn->opcode();

    auto default_case = [&]() {
      if (insn->has_dest()) {
        env->set(insn->dest(), UInt32ConstantDomain::top());
        if (insn->dest_is_wide()) {
          env->set(insn->dest() + 1, UInt32ConstantDomain::top());
        }
      } else if (insn->has_move_result_any()) {
        env->set(RESULT_REGISTER, UInt32ConstantDomain::top());
      }
    };

    switch (op) {
    case IOPCODE_LOAD_PARAM:
    case IOPCODE_LOAD_PARAM_WIDE:
    case IOPCODE_LOAD_PARAM_OBJECT: {
      not_reached_log("<clinit> is static and doesn't take any arguments");
    }

    case OPCODE_MOVE:
    case OPCODE_MOVE_OBJECT:
    case OPCODE_MOVE_WIDE: {
      analyze_move_const(insn, env);
      break;
    }

    case OPCODE_CONST:
    case OPCODE_CONST_WIDE: {
      env->set(
          insn->dest(),
          UInt32ConstantDomain(static_cast<uint32_t>(insn->get_literal())));
      break;
    }

    default: {
      default_case();
      break;
    }
    }
  }
};

} // namespace impl

OptimizeEnumsGeneratedAnalysis::~OptimizeEnumsGeneratedAnalysis() {}

OptimizeEnumsGeneratedAnalysis::OptimizeEnumsGeneratedAnalysis(
    const DexClass* generated_cls, const DexType* current_enum)
    : m_enum(current_enum), m_generated_cls(generated_cls) {
  auto clinit = generated_cls->get_clinit();
  always_assert(clinit && clinit->get_code());

  m_clinit_cfg = cfg::ScopedCFG(clinit->get_code());
  m_clinit_cfg->calculate_exit_block();

  m_field_analyzer = std::make_unique<impl::FieldAnalyzer>(
      *m_clinit_cfg, generated_cls, current_enum);
  m_const_analyzer = std::make_unique<impl::ConstAnalyzer>(*m_clinit_cfg);
}

void OptimizeEnumsGeneratedAnalysis::collect_generated_switch_cases(
    GeneratedSwitchCases& generated_switch_cases) {
  for (cfg::Block* block : m_clinit_cfg->blocks()) {
    auto const_env = m_const_analyzer->get_entry_state_at(block);
    auto field_env = m_field_analyzer->get_entry_state_at(block);

    for (const auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;

      if (insn->opcode() == OPCODE_APUT) {
        reg_t input_reg = insn->src(0);
        reg_t lookup_table_reg = insn->src(1);
        reg_t ordinal_reg = insn->src(2);

        auto lookup_table = field_env.get(lookup_table_reg).get_constant();
        auto field_ordinal = field_env.get(ordinal_reg).get_constant();
        auto switch_case = const_env.get(input_reg).get_constant();

        // We expect APUTs only for the lookup table.
        always_assert(lookup_table && switch_case);
        always_assert((*lookup_table)->get_class() ==
                      m_generated_cls->get_type());
        if (!field_ordinal || (*field_ordinal)->get_class() != m_enum) {
          continue;
        }

        // Set switch case, based on the value that is associated with the enum
        // field.
        generated_switch_cases[*lookup_table][*switch_case] = *field_ordinal;
      }

      m_field_analyzer->analyze_instruction(insn, &field_env);
      m_const_analyzer->analyze_instruction(insn, &const_env);
    }
  }
}

} // namespace optimize_enums
