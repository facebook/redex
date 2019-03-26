/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "EnumInSwitch.h"
#include "Resolver.h"

namespace optimize_enums {

bool has_all_but_branch(boost::optional<Info> info) {
  return info && info->array_field && info->invoke && info->aget;
}

// If exactly one of the input registers is holding the case key for an enum,
// return an info struct with that register filled in. Otherwise, return none.
boost::optional<Info> get_enum_reg(const Environment& env,
                                   IRInstruction* insn) {
  if (insn->srcs_size() == 1) {
    // *-switch or if-*z
    auto reg = insn->src(0);
    auto info = env.get(reg).get_constant();
    if (has_all_but_branch(info)) {
      info->reg = reg;
      return info;
    }
  } else {
    // if-* v1 v2
    // Only one of the registers should have the case key in it. Return that
    // one. If both registers do then return none.
    always_assert(insn->srcs_size() == 2);
    uint16_t l_reg = insn->src(0);
    uint16_t r_reg = insn->src(1);
    boost::optional<Info> left = env.get(l_reg).get_constant();
    boost::optional<Info> right = env.get(r_reg).get_constant();
    bool l_has = has_all_but_branch(left);
    bool r_has = has_all_but_branch(right);
    if (l_has && !r_has) {
      left->reg = l_reg;
      return left;
    } else if (!l_has && r_has) {
      right->reg = r_reg;
      return right;
    }
  }
  return boost::none;
}

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

void analyze_branch(cfg::InstructionIterator it, Environment* env) {
  auto insn = it->insn;
  auto info = get_enum_reg(*env, insn);
  if (has_all_but_branch(info)) {
    info->branch = it;
    env->set(*info->reg, Domain(*info));
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
      const auto& cfg_it = b->to_cfg_instruction_iterator(it);
      if (is_branch(op)) {
        auto info = get_enum_reg(env, insn);
        if (info && info->branch == boost::none) {
          // We check to make sure info.branch is none because we want to only
          // get the first branch of the if-else chain.
          info->branch = cfg_it;
          result.emplace_back(*info);
        }
      }
      analyze_insn(cfg_it, &env);
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
  case OPCODE_PACKED_SWITCH:
  case OPCODE_SPARSE_SWITCH:
  case OPCODE_IF_EQ:
  case OPCODE_IF_NE:
  case OPCODE_IF_LT:
  case OPCODE_IF_GE:
  case OPCODE_IF_GT:
  case OPCODE_IF_LE:
  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ:
  case OPCODE_IF_LTZ:
  case OPCODE_IF_GEZ:
  case OPCODE_IF_GTZ:
  case OPCODE_IF_LEZ:
    analyze_branch(it, env);
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
