/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantValue.h"

namespace {

std::vector<IRInstruction*> make_string_const(uint16_t dest, std::string val) {
  std::vector<IRInstruction*> res;
  IRInstruction* load = new IRInstruction(OPCODE_CONST_STRING);
  load->set_string(DexString::make_string(val));
  IRInstruction* move_result_pseudo =
      new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
  move_result_pseudo->set_dest(dest);
  res.push_back(load);
  res.push_back(move_result_pseudo);
  return res;
}

} // namespace

ConstantValue::ConstantValue(const TypeTags* type_tags,
                             std::string kind_str,
                             std::string val_str) {
  if (kind_str == "I") {
    m_kind = ConstantKind::INT;
    m_int_val = std::stoll(val_str);
  } else if (kind_str == "T") {
    auto type_val = DexType::get_type(val_str.c_str());
    if (type_val != nullptr && type_tags->has_type_tag(type_val)) {
      m_kind = ConstantKind::TYPE;
      m_int_val = type_tags->get_type_tag(type_val);
      return;
    } else if (type_val == nullptr) {
      TRACE(TERA, 9, "const lift: unable to find type %s\n", val_str.c_str());
    }
    // Cannot find type or not type tag.
    m_kind = ConstantKind::INVALID;
  } else if (kind_str == "S") {
    m_kind = ConstantKind::STRING;
    m_str_val = val_str;
  } else {
    always_assert_log(false, "Unexpected kind str %s\n", kind_str.c_str());
  }
}

std::vector<ConstantValue::ConstantLoad>
ConstantValue::collect_constant_loads_in(IRCode* code) {
  std::vector<ConstantValue::ConstantLoad> res;
  if (is_invalid()) {
    return res;
  }
  auto ii = InstructionIterable(code);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    auto insn = it->insn;
    if (is_int_value() && is_literal_const(insn->opcode())) {
      int64_t literal = insn->get_literal();
      // Special handling for type tags to avoid sign extensionon on int64_t.
      if (m_kind == ConstantKind::TYPE) {
        literal = static_cast<uint32_t>(literal);
      }
      if (is_int_value() && literal == m_int_val) {
        res.emplace_back(insn, insn->dest());
      }
    } else if (is_str_value() && insn->opcode() == OPCODE_CONST_STRING) {
      if (strcmp(insn->get_string()->c_str(), m_str_val.c_str()) == 0) {
        auto pseudo_move = std::next(it)->insn;
        always_assert(pseudo_move->opcode() ==
                      IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
        res.emplace_back(insn, pseudo_move->dest());
      }
    }
  }

  return res;
}

std::vector<IRInstruction*> ConstantValue::make_load_const(uint16_t const_reg) {
  always_assert(!is_invalid());

  if (is_int_value()) {
    std::vector<IRInstruction*> res;
    auto load = method_reference::make_load_const(const_reg, m_int_val);
    res.push_back(load);
    return res;
  } else {
    return make_string_const(const_reg, m_str_val);
  }
}
