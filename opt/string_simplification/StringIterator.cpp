/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexAsm.h"
#include "DexInstruction.h"

#include "StringIterator.h"

using NodeId = cfg::Block*;
using Environment = StringProdEnvironment;

template <typename T>
static const std::string showd(const T& val) {
  std::ostringstream ss;
  ss << val;
  return ss.str();
}

IRList::iterator next_insn(IRList::iterator it) {
  auto future = std::next(it);
  while (future->type != MFLOW_OPCODE) {
    future = std::next(future);
  }
  return future;
}

IRList::iterator next_insn(IRList::iterator it, cfg::Block* blk) {
  auto future = std::next(it);
  while (future->type != MFLOW_OPCODE && future != blk->end()) {
    future = std::next(future);
  }
  return future;
}

IRList::iterator prev_insn(IRList::iterator it) {
  auto past = std::prev(it);
  while (past->type != MFLOW_OPCODE) {
    past = std::prev(past);
  }
  return past;
}

bool eq(const PointerDomain& a, const PointerDomain& b) {
  if (a.is_top()) {
    return b.is_top();
  } else if (a.is_bottom()) {
    return b.is_bottom();
  } else {
    return a.value() == b.value();
  }
}

void StringIterator::analyze_instruction(const NodeId blk,
                                         IRList::iterator& it,
                                         Environment* env) const {
  always_assert(it->type == MFLOW_OPCODE);
  auto insn = it->insn;
  TRACE(STR_SIMPLE, 8, "insn: %s\n", SHOW(insn));

  if (is_const_string(it)) {
    const std::string& s = insn->get_string()->str();
    env->create(insn->dest());
    env->put(insn->dest(), StringyDomain::value(s, boost::none, true));

  } else if (is_sb_new_instance(it)) {
    env->create(insn->dest());

  } else if (is_sb_empty_init(it)) {
    env->put(insn->src(0), StringyDomain::value(""));

  } else if (is_sb_string_init(it)) {
    auto rhs_abstract = env->eval(insn->src(1));
    if (rhs_abstract.is_value() && rhs_abstract.value().is_static_string()) {
      auto s = rhs_abstract.value().suffix();
      env->put(insn->src(0), StringyDomain::value(s));
    } else {
      env->put(insn->src(0), StringyDomain::top());
    }

  } else if (is_sb_append_string(it)) {
    auto sb_abstract = env->eval(insn->src(0));
    auto rhs_abstract = env->eval(insn->src(1));
    if (sb_abstract.is_value() && rhs_abstract.is_value() &&
        rhs_abstract.value().is_static_string()) {
      auto result = StringyDomain::append(
          sb_abstract, it->insn->src(0), rhs_abstract.value().suffix());
      env->put(it->insn->src(0), result);
    } else {
      env->put(it->insn->src(0), StringyDomain::top());
    }

    auto future = next_insn(it, blk);
    if (future != blk->end() &&
        future->insn->opcode() == OPCODE_MOVE_RESULT_OBJECT) {
      env->move(future->insn->dest(), it->insn->src(0));
      it = future;
    }

  } else if (is_sb_to_string(it)) {
    TRACE(STR_SIMPLE, 6, "found StringBuilder.toString()\n");

  } else if (is_invoke(insn->opcode())) {
    // Set all call's to top if not static strings.
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      if (!env->is_tracked(insn->src(i))) {
        continue;
      }
      auto val = env->eval(insn->src(i));
      if (val.kind() != AbstractValueKind::Value ||
          !val.value().is_static_string()) {
        env->put(insn->src(i), StringyDomain::top());
      }
    }

  } else if (is_move(it->insn->opcode())) {
    env->move(it->insn->dest(), it->insn->src(0));
    if (it->insn->src_is_wide(0)) {
      env->clear(it->insn->dest() + 1);
    }

  } else { // Any other instruction.
    if (it->insn->dests_size()) {
      env->clear(it->insn->dest());
      if (it->insn->dest_is_wide()) {
        env->clear(it->insn->dest() + 1);
      }
    }
  }
  TRACE(STR_SIMPLE, 8, "env: %s\n", showd(*env).c_str());
}

void StringIterator::simplify_instruction(const NodeId block,
                                          IRList::iterator& it,
                                          const Environment* current_state) {
  auto insn = it->insn;
  if (!is_sb_to_string(it)) {
    return;
  }

  const auto sb_reg = insn->src(0);
  const auto sb_abstract = current_state->eval(sb_reg);
  always_assert(!sb_abstract.is_bottom());

  TRACE(STR_SIMPLE, 4, "Simplifying toString()\n");
  if (sb_abstract.is_top() || sb_abstract.value().has_base()) {
    TRACE(STR_SIMPLE, 4, "Aborting, no information known.\n");
    return;
  }
  TRACE(STR_SIMPLE, 4, "value: %s\n", showd(sb_abstract).c_str());

  auto future = next_insn(it);
  if (future->insn->opcode() != OPCODE_MOVE_RESULT_OBJECT) {
    remove_stringbuilder_instructions_in_block(
        it, current_state, block, sb_reg);
    return;
  }

  if (!sb_abstract.value().has_base()) {
    always_assert(future->insn->opcode() == OPCODE_MOVE_RESULT_OBJECT);
    auto result_reg = future->insn->dest();
    future = m_code->erase(future);
    remove_stringbuilder_instructions_in_block(
        it, current_state, block, sb_reg);

    auto final_string = sb_abstract.value().suffix();
    insert_const_string(it, result_reg, final_string);
    TRACE(STR_SIMPLE, 5, "pushed constant: %s\n", final_string.c_str());

  } else if (sb_abstract.value().suffix() == "") {
    using namespace dex_asm;

    always_assert(future->insn->opcode() == OPCODE_MOVE_RESULT_OBJECT);
    auto result_reg = future->insn->dest();

    auto base_reg = sb_abstract.value().base();
    TRACE(STR_SIMPLE, 1, "Warning: possibly empty stringbuilder.\n");
    m_code->insert_after(
        future,
        dasm(OPCODE_MOVE_OBJECT, {{VREG, result_reg}, {VREG, base_reg}}));
    ++m_instructions_added;
    TRACE(STR_SIMPLE, 5, "pushed move.\n");

  } else {
    always_assert(future->insn->opcode() == OPCODE_MOVE_RESULT_OBJECT);
    auto result_reg = future->insn->dest();
    future = m_code->erase(future);
    remove_stringbuilder_instructions_in_block(
        it, current_state, block, sb_reg);

    auto base_reg = sb_abstract.value().base();

    insert_sb_init(it, sb_reg);
    insert_sb_append(it, sb_reg, base_reg);

    auto free_reg = m_code->allocate_temp();
    insert_const_string(it, free_reg, sb_abstract.value().suffix());
    insert_sb_append(it, sb_reg, free_reg);

    insert_sb_to_string(it, sb_reg, result_reg);
    TRACE(STR_SIMPLE, 5, "pushed simplified StringBuilder.\n");
  }
}

void StringIterator::remove_stringbuilder_instructions_in_block(
    IRList::iterator& it,
    const Environment* c_env,
    NodeId block,
    string_register_t sb_reg) {
  Environment env = *c_env;

  // Unique pointer id in heap, so we can make sure we catch register aliasing
  // (other registers that point to this stringbuilder object).
  auto id = env.get_id(sb_reg);
  always_assert(id.is_value());

  if (it == block->begin()) {
    TRACE(STR_SIMPLE, 1, "toString at beginning of block.\n");
    m_code->remove_opcode(it);
    it = m_code->insert_after(it);
    ++m_instructions_added;
    ++m_instructions_removed;
    return;
  }

  it = m_code->erase(it); // Erase the toString.
  ++m_instructions_removed;

  auto back_iter = std::prev(it);
  while (back_iter != block->begin()) {
    bool should_erase = false;
    if (back_iter->type != MFLOW_OPCODE) {
      back_iter = std::prev(back_iter);
      continue;
    }

    if (back_iter->insn->dests_size() &&
        eq(env.get_id(back_iter->insn->dest()), id)) {
      if (back_iter->insn->opcode() == OPCODE_NEW_INSTANCE) {
        TRACE(STR_SIMPLE, 5, "new instance.\n");
        always_assert(back_iter->insn->get_type() == m_builder_type);
        back_iter = m_code->erase(back_iter);
        ++m_instructions_removed;
        break;
      }
      should_erase = true;
    }

    // In the analysis it should catch all invoke's that aren't defined (aka not
    // something like append/toString) and then to set the StringBuilder as top,
    // so we wouldn't optimize this case, and wouldn't have to worry about it
    // when removing instructions.
    if ((back_iter->insn->opcode() == OPCODE_INVOKE_VIRTUAL ||
         back_iter->insn->opcode() == OPCODE_INVOKE_DIRECT) &&
        eq(env.get_id(back_iter->insn->src(0)), id)) {
      should_erase = true;
    }

    if (back_iter->insn->opcode() == OPCODE_MOVE_RESULT_OBJECT &&
        eq(env.get_id(back_iter->insn->dest()), id)) {
      auto past = prev_insn(back_iter);

      if (is_sb_append_string(past)) {
        should_erase = true;
        TRACE(STR_SIMPLE,
              5,
              "propagating: %d to %d\n",
              sb_reg,
              past->insn->src(0));
        env.move(past->insn->src(0), sb_reg);
      }
    }

    if (should_erase) {
      back_iter = m_code->erase(back_iter);
      ++m_instructions_removed;
    }
    back_iter = std::prev(back_iter);
  }
}

//========== dasm helpers ==========

bool StringIterator::is_const_string(IRList::iterator it) const {
  return it->insn->opcode() == OPCODE_CONST_STRING;
}

bool StringIterator::is_sb_new_instance(IRList::iterator it) const {
  auto insn = it->insn;
  return insn->opcode() == OPCODE_NEW_INSTANCE &&
         insn->get_type() == m_builder_type;
}

bool StringIterator::is_sb_empty_init(IRList::iterator it) const {
  auto insn = it->insn;
  return insn->opcode() == OPCODE_INVOKE_DIRECT &&
         insn->get_method() ==
             DexMethod::make_method(STRINGBUILDER_DEF, "<init>", "V", {});
}

bool StringIterator::is_sb_string_init(IRList::iterator it) const {
  auto insn = it->insn;
  return insn->opcode() == OPCODE_INVOKE_DIRECT &&
         insn->get_method() ==
             DexMethod::make_method(
                 STRINGBUILDER_DEF, "<init>", "V", {STRING_DEF});
}

bool StringIterator::is_sb_append_string(IRList::iterator it) const {
  auto insn = it->insn;
  return insn->opcode() == OPCODE_INVOKE_VIRTUAL &&
         insn->get_method() == m_append_method;
}

bool StringIterator::is_sb_to_string(IRList::iterator it) const {
  return it->insn->opcode() == OPCODE_INVOKE_VIRTUAL &&
         it->insn->get_method() == m_to_string_method;
}

void StringIterator::insert_sb_init(IRList::iterator& it, uint16_t vreg) {
  using namespace dex_asm;
  m_code->insert_before(it,
                        dasm(OPCODE_NEW_INSTANCE,
                             DexType::make_type(STRINGBUILDER_DEF),
                             {{VREG, vreg}}));
  m_code->insert_before(
      it,
      dasm(OPCODE_INVOKE_DIRECT,
           DexMethod::make_method(STRINGBUILDER_DEF, "<init>", "V", {}),
           {{VREG, vreg}}));
  m_instructions_added += 2;
}

void StringIterator::insert_sb_append(IRList::iterator& it,
                                      uint16_t sb_vreg,
                                      uint16_t str_vreg) {
  using namespace dex_asm;
  m_code->insert_before(it,
                        dasm(OPCODE_INVOKE_VIRTUAL,
                             m_append_method,
                             {{VREG, sb_vreg}, {VREG, str_vreg}}));
  ++m_instructions_added;
}

void StringIterator::insert_const_string(IRList::iterator& it,
                                         uint16_t dest,
                                         std::string s) {
  using namespace dex_asm;
  m_code->insert_before(
      it, dasm(OPCODE_CONST_STRING, DexString::make_string(s), {{VREG, dest}}));
  ++m_instructions_added;
  ++m_strings_added;
}

void StringIterator::insert_sb_to_string(IRList::iterator& it,
                                         uint16_t sb_vreg,
                                         uint16_t dest_vreg) {
  using namespace dex_asm;
  m_code->insert_before(
      it, dasm(OPCODE_INVOKE_VIRTUAL, m_to_string_method, {{VREG, sb_vreg}}));
  m_code->insert_before(it,
                        dasm(OPCODE_MOVE_RESULT_OBJECT, {{VREG, dest_vreg}}));
  m_instructions_added += 2;
}
