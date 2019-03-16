/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "EnumInSwitch.h"
#include "Resolver.h"

namespace optimize_enums {

void analyze_default(cfg::InstructionIterator it, Environment* env) {
  auto insn = it->insn;
  if (insn->dests_size()) {
    env->set(insn->dest(), Domain::top());
    if (insn->dest_is_wide()) {
      env->set(insn->dest() + 1, Domain::top());
    }
  }
  if (insn->has_move_result()) {
    env->set(RESULT_REGISTER, Domain::top());
  }
}

void analyze_sget(cfg::InstructionIterator it, Environment* env) {
  auto insn = it->insn;
  auto op = insn->opcode();
  if (op == OPCODE_SGET_OBJECT) {
    auto field = resolve_field(insn->get_field(), FieldSearch::Static);
    if (field != nullptr) {
      Info info;
      info.array_field = field;
      env->set(RESULT_REGISTER, Domain(info));
      return;
    }
  }
  analyze_default(it, env);
}

void analyze_invoke(cfg::InstructionIterator it, Environment* env) {
  auto insn = it->insn;
  DexMethodRef* ref = insn->get_method();
  if (ref->get_name()->str() == "ordinal") {
    // All methods named `ordinal` is overly broad, but we throw out false
    // positives later
    Info info;
    info.invoke = it;
    env->set(RESULT_REGISTER, Domain(info));
  } else {
    analyze_default(it, env);
  }
}

void analyze_aget(cfg::InstructionIterator it, Environment* env) {
  auto insn = it->insn;
  auto info = env->get(insn->src(0)).get_constant();
  const auto& invoke_info = env->get(insn->src(1)).get_constant();
  if (info && invoke_info && info->array_field != nullptr &&
      invoke_info->invoke != boost::none) {
    // Combine the information from each register.
    // Later, we'll make sure array_field is of the right enum
    info->invoke = invoke_info->invoke;
    info->aget = it;
    env->set(RESULT_REGISTER, Domain(*info));
  } else {
    analyze_default(it, env);
  }
}

void analyze_move_result(cfg::InstructionIterator it, Environment* env) {
  auto insn = it->insn;
  if (!insn->dest_is_wide()) {
    env->set(insn->dest(), env->get(RESULT_REGISTER));
  } else {
    analyze_default(it, env);
  }
}

void analyze_move(cfg::InstructionIterator it, Environment* env) {
  auto insn = it->insn;
  auto op = insn->opcode();
  if (op == OPCODE_MOVE) {
    env->set(insn->dest(), env->get(insn->src(0)));
  } else {
    analyze_default(it, env);
  }
}

void Iterator::analyze_node(cfg::Block* const& block, Environment* env) const {
  auto ii = ir_list::InstructionIterable(*block);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    auto cfg_it = block->to_cfg_instruction_iterator(it);
    analyze_insn(cfg_it, env);
  }
}

std::vector<Info> Iterator::collect() const {
  std::vector<Info> result;
  for (cfg::Block* b : m_cfg->blocks()) {
    Environment env = get_entry_state_at(b);
    auto ii = ir_list::InstructionIterable(*b);
    for (auto it = ii.begin(); it != ii.end(); ++it) {
      auto insn = it->insn;
      auto op = insn->opcode();
      auto cfg_it = b->to_cfg_instruction_iterator(it);
      if (is_switch(op)) {
        auto info = env.get(insn->src(0)).get_constant();
        if (info && info->array_field != nullptr &&
            info->invoke != boost::none && info->aget != boost::none) {
          info->switch_ordinal = cfg_it;
          result.push_back(*info);
        }
      } else {
        analyze_insn(cfg_it, &env);
      }
    }
  }
  return result;
}

void Iterator::analyze_insn(cfg::InstructionIterator it,
                            Environment* env) const {
  auto op = it->insn->opcode();
  switch (op) {
  case OPCODE_MOVE:
  case OPCODE_MOVE_WIDE:
  case OPCODE_MOVE_OBJECT:
    analyze_move(it, env);
    break;
  case OPCODE_MOVE_RESULT:
  case OPCODE_MOVE_RESULT_WIDE:
  case OPCODE_MOVE_RESULT_OBJECT:
  case IOPCODE_MOVE_RESULT_PSEUDO:
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
    analyze_move_result(it, env);
    break;
  case OPCODE_AGET:
  case OPCODE_AGET_WIDE:
  case OPCODE_AGET_OBJECT:
  case OPCODE_AGET_BOOLEAN:
  case OPCODE_AGET_BYTE:
  case OPCODE_AGET_CHAR:
  case OPCODE_AGET_SHORT:
    analyze_aget(it, env);
    break;
  case OPCODE_SGET:
  case OPCODE_SGET_WIDE:
  case OPCODE_SGET_OBJECT:
  case OPCODE_SGET_BOOLEAN:
  case OPCODE_SGET_BYTE:
  case OPCODE_SGET_CHAR:
  case OPCODE_SGET_SHORT:
    analyze_sget(it, env);
    break;
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE:
    analyze_invoke(it, env);
    break;
  default:
    analyze_default(it, env);
    break;
  }
}

Environment Iterator::analyze_edge(
    cfg::Edge* const&, const Environment& exit_state_at_source) const {
  return exit_state_at_source;
}

} // namespace optimize_enums
