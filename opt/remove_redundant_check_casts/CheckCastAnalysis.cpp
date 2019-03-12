/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CheckCastAnalysis.h"

#include "DexUtil.h"
#include "Resolver.h"

namespace check_casts {

namespace impl {

using namespace ir_analyzer;

void CheckCastAnalysis::analyze_instruction(IRInstruction* insn,
                                            Environment* current_state) const {

  auto default_case = [&]() {
    // If we get here, reset destination.
    if (insn->dests_size()) {
      current_state->set(insn->dest(), Domain::top());
      if (insn->dest_is_wide()) {
        current_state->set(insn->dest() + 1, Domain::top());
      }
    } else if (insn->has_move_result() || insn->has_move_result_pseudo()) {
      // Here we don't need to update RESULT_REGISTER + 1 for wide cases,
      // since we only care about keeping track of objects, which are not wide.
      current_state->set(RESULT_REGISTER, Domain::top());
    }
  };

  switch (insn->opcode()) {
  case OPCODE_MOVE_OBJECT: {
    current_state->set(insn->dest(), current_state->get(insn->src(0)));
    break;
  }

  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
  case OPCODE_MOVE_RESULT_OBJECT: {
    current_state->set(insn->dest(), current_state->get(RESULT_REGISTER));
    break;
  }

  case OPCODE_NEW_INSTANCE:
  case OPCODE_CHECK_CAST: {
    DexType* type = insn->get_type();
    current_state->set(RESULT_REGISTER, Domain(type));
    break;
  }

  case OPCODE_IGET_OBJECT:
  case OPCODE_SGET_OBJECT: {
    DexFieldRef* field = insn->get_field();
    DexType* field_type = field != nullptr ? field->get_type() : nullptr;
    if (field_type && field_type != get_string_type()) {
      current_state->set(RESULT_REGISTER, Domain(field->get_type()));
    } else {
      default_case();
    }
    break;
  }

  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE: {
    auto method = insn->get_method();
    auto rtype = method->get_proto()->get_rtype();
    if (rtype) {
      if (!is_primitive(rtype) && rtype != get_string_type()) {
        current_state->set(RESULT_REGISTER, Domain(rtype));
      } else {
        default_case();
      }
    }
    break;
  }

  default: {
    default_case();
    break;
  }
  }
}

std::unordered_map<IRInstruction*, boost::optional<IRInstruction*>>
CheckCastAnalysis::collect_redundant_checks_replacement() {
  auto* code = m_method->get_code();
  auto& cfg = code->cfg();

  std::unordered_map<IRInstruction*, boost::optional<IRInstruction*>> insns;

  for (cfg::Block* block : cfg.blocks()) {
    auto env = get_entry_state_at(block);

    auto ii = InstructionIterable(block);
    for (auto it = ii.begin(); it != ii.end(); it++) {
      IRInstruction* insn = it->insn;
      if (insn->opcode() == OPCODE_CHECK_CAST) {
        auto type_opt = env.get(insn->src(0)).get_constant();
        if (type_opt && check_cast(*type_opt, insn->get_type())) {
          auto src = insn->src(0);
          auto dst = ir_list::move_result_pseudo_of(it.unwrap())->dest();
          if (src == dst) {
            insns[insn] = boost::none;
          } else {
            auto new_move = new IRInstruction(OPCODE_MOVE_OBJECT);
            new_move->set_src(0, src);
            new_move->set_dest(dst);
            insns[insn] = boost::optional<IRInstruction*>(new_move);
          }
        }
      }
      analyze_instruction(insn, &env);
    }
  }

  return insns;
}

} // namespace impl

} // namespace check_casts
