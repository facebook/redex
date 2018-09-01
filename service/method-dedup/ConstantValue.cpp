/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantValue.h"

#include <boost/algorithm/string.hpp>

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
                             const std::string kind_str,
                             const std::string val_str,
                             const uint16_t param_reg)
    : m_param_reg(param_reg) {
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
      TRACE(TERA, 9, "const value: unable to find type %s\n", val_str.c_str());
    } else {
      TRACE(TERA, 9, "const value: no type tag found %s\n", val_str.c_str());
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
ConstantValue::collect_constant_loads_in(const IRCode* code) {
  std::vector<ConstantValue::ConstantLoad> res;
  always_assert(is_valid());
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
  always_assert(is_valid());

  if (is_int_value()) {
    std::vector<IRInstruction*> res;
    auto load = method_reference::make_load_const(const_reg, m_int_val);
    res.push_back(load);
    return res;
  } else {
    return make_string_const(const_reg, m_str_val);
  }
}

ConstantValues::ConstantValues(const TypeTags* type_tags,
                               const std::string kinds_str,
                               const std::string vals_str,
                               IRCode* code) {
  // Split vals_str.
  std::vector<std::string> vals_vec;
  boost::split(vals_vec, vals_str, [](char c) { return c == ':'; });
  always_assert(vals_vec.size() == kinds_str.length());

  // Build kind_to_val pairs.
  std::vector<std::pair<std::string, std::string>> kind_to_val;
  auto vals_it = vals_vec.begin();
  for (std::string::const_iterator it = kinds_str.begin();
       it != kinds_str.end();
       ++it) {
    std::string kind_str = std::string(1, *it);
    kind_to_val.emplace_back(kind_str, *vals_it);
    ++vals_it;
  }

  // Populate the const_vals set.
  for (auto& pair : kind_to_val) {
    auto param_reg = code->allocate_temp();
    ConstantValue cval(type_tags, pair.first, pair.second, param_reg);
    m_const_vals.emplace_back(cval);
    if (cval.is_invalid()) {
      m_skip_multiple_const_0 = true;
      TRACE(TERA, 9, "const value: skip multiple const 0\n");
    }
  }
}

std::vector<ConstantValues::ConstantValueLoad>
ConstantValues::collect_constant_loads(const IRCode* code) {
  std::vector<ConstantValueLoad> const_val_loads;
  for (auto& const_val : m_const_vals) {
    if (const_val.is_invalid()) {
      continue;
    }
    auto const_loads = const_val.collect_constant_loads_in(code);
    if (m_skip_multiple_const_0 && const_val.is_int_kind() &&
        const_val.get_int_value() == 0 && !const_loads.empty()) {
      const_val_loads.emplace_back(const_val, const_loads.front());
      TRACE(TERA, 9, "const value: skip const 0 loads\n");
      continue;
    }
    for (auto& load : const_loads) {
      const_val_loads.emplace_back(const_val, load);
    }
  }
  return const_val_loads;
}

std::vector<IRInstruction*> ConstantValues::make_const_loads(
    std::vector<uint16_t>& const_regs) {
  always_assert(const_regs.size() == size());
  std::vector<IRInstruction*> res;
  size_t reg_idx = 0;
  for (auto& cval : m_const_vals) {
    if (cval.is_valid()) {
      auto loads = cval.make_load_const(const_regs.at(reg_idx++));
      res.insert(res.end(), loads.begin(), loads.end());
    }
  }
  return res;
}
