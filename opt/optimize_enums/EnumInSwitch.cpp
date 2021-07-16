/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "EnumInSwitch.h"
#include "Resolver.h"
#include "Trace.h"

namespace optimize_enums {

bool has_all_but_branch(const Domain& domain) {
  if (domain.is_top() || domain.is_bottom()) {
    return false;
  }
  auto info = domain.get_info();
  return info && info->array_field && info->invoke && info->aget;
}

// If exactly one of the input registers is holding the case key for an enum or
// a known constant, return the domain with that register filled in. Otherwise,
// return none.
boost::optional<Domain> get_enum_reg(const Environment& env,
                                     IRInstruction* insn) {
  if (insn->srcs_size() == 1) {
    // *-switch or if-*z
    auto reg = insn->src(0);
    auto& domain = env.get(reg);
    TRACE(ENUM, 9, "insn %s\n\t%s", SHOW(insn), SHOW(env.get(reg)));
    if (has_all_but_branch(domain)) {
      return domain.combine_with_reg(reg);
    }
  } else {
    // if-* v1 v2
    // Only one of the registers should have the case key in it. Return that
    // one. If both registers do then return none.
    always_assert(insn->srcs_size() == 2);
    reg_t l_reg = insn->src(0);
    reg_t r_reg = insn->src(1);
    auto& l_domain = env.get(l_reg);
    auto& r_domain = env.get(r_reg);
    bool l_has = has_all_but_branch(l_domain);
    bool r_has = has_all_but_branch(r_domain);
    if (l_has && !r_has) {
      return l_domain.combine_with_reg(l_reg);
    } else if (!l_has && r_has) {
      return r_domain.combine_with_reg(r_reg);
    }
  }
  return boost::none;
}

void analyze_default(const cfg::InstructionIterator& it, Environment* env) {
  auto insn = it->insn;
  if (insn->has_dest()) {
    env->set(insn->dest(), Domain::top());
    if (insn->dest_is_wide()) {
      env->set(insn->dest() + 1, Domain::top());
    }
  }
  if (insn->has_move_result_any()) {
    env->set(RESULT_REGISTER, Domain::top());
  }
}

void analyze_sget(const cfg::InstructionIterator& it, Environment* env) {
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
  auto domain = get_enum_reg(*env, insn);
  if (domain && has_all_but_branch(*domain)) {
    env->set(*domain->get_info()->reg, domain->combine_with_branch(it));
  } else {
    analyze_default(it, env);
  }
}

void analyze_aget(cfg::InstructionIterator it, Environment* env) {
  auto insn = it->insn;
  TRACE(ENUM, 9, "insn %s\n\t%s", SHOW(insn), SHOW(env->get(insn->src(0))));
  auto info = env->get(insn->src(0)).get_info();
  const auto& invoke_info = env->get(insn->src(1)).get_info();
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

void analyze_move_result(const cfg::InstructionIterator& it, Environment* env) {
  auto insn = it->insn;
  if (!insn->dest_is_wide()) {
    env->set(insn->dest(), env->get(RESULT_REGISTER));
  } else {
    analyze_default(it, env);
  }
}

void analyze_move(const cfg::InstructionIterator& it, Environment* env) {
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

std::vector<EnumSwitchKey> Iterator::collect() const {
  std::vector<EnumSwitchKey> result;

  for (cfg::Block* b : m_cfg->blocks()) {
    Environment env = get_entry_state_at(b);
    auto ii = ir_list::InstructionIterable(*b);
    for (auto it = ii.begin(); it != ii.end(); ++it) {
      auto insn = it->insn;
      auto op = insn->opcode();
      const auto& cfg_it = b->to_cfg_instruction_iterator(it);
      if (opcode::is_branch(op)) {
        if (auto domain = get_enum_reg(env, insn)) {
          auto info = domain->get_info();
          if (info->branch == boost::none) {
            // We check to make sure info.branch is none because we want to only
            // get the first branch of the if-else chain.
            info->branch = cfg_it;
            // Check the other possible constant.
            auto another_constant_key = domain->get<1>();
            if (another_constant_key.is_top()) {
              TRACE(ENUM,
                    9,
                    "Unknown value flows into EnumSwitch in %s",
                    SHOW(*m_cfg));
              return {};
            } else {
              auto key2 = domain->get_constant();
              if (key2 && *key2 >= 0) {
                TRACE(ENUM,
                      9,
                      "key %d may be conflict with EnumSwitch keys in %s",
                      *key2,
                      SHOW(*m_cfg));
                return {};
              } else {
                TRACE(ENUM, 9, "%s", SHOW(*info));
                result.emplace_back(std::make_pair(*info, key2));
              }
            }
          }
        }
      }
      analyze_insn(cfg_it, &env);
    }
  }
  return result;
}

void Iterator::analyze_insn(const cfg::InstructionIterator& it,
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
  case OPCODE_SWITCH:
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
  case OPCODE_CONST:
    env->set(it->insn->dest(), Domain(it->insn->get_literal()));
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
