/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantValue.h"

#include <boost/algorithm/string.hpp>

#include "Creators.h"
#include "TypeReference.h"

namespace {

constexpr uint64_t MAX_NUM_CONST_VALUE = 10;

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
      TRACE(METH_DEDUP,
            9,
            "const value: unable to find type %s\n",
            val_str.c_str());
    } else {
      TRACE(METH_DEDUP,
            9,
            "const value: no type tag found %s\n",
            val_str.c_str());
    }
    // Cannot find type or not type tag.
    m_kind = ConstantKind::INVALID;
  } else if (kind_str == "S") {
    m_kind = ConstantKind::STRING;
    m_str_val = val_str;
  } else if (kind_str.size() > 1) {
    TRACE(METH_DEDUP,
          9,
          "const lift: trying to decode more than one kind %s\n",
          kind_str.c_str());
    m_kind = ConstantKind::INVALID;
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
                               const size_t stud_method_threshold,
                               IRCode* code)
    : m_stub_method_threshold(stud_method_threshold) {
  // Split vals_str.
  std::vector<std::string> vals_vec;
  boost::split(vals_vec, vals_str, [](char c) { return c == ':'; });
  always_assert(vals_vec.size() == kinds_str.length());

  if (kinds_str.size() > MAX_NUM_CONST_VALUE) {
    TRACE(METH_DEDUP,
          8,
          "const value: skip large number of const values %ld\n",
          kinds_str.size());
    return;
  }

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
      TRACE(METH_DEDUP, 9, "const value: skip multiple const 0\n");
    }
  }
}

std::vector<ConstantValues::ConstantValueLoad>
ConstantValues::collect_constant_loads(const IRCode* code) {
  std::vector<ConstantValueLoad> const_val_loads;
  std::unordered_set<IRInstruction*> matched_loads;
  for (auto& const_val : m_const_vals) {
    if (const_val.is_invalid()) {
      continue;
    }
    auto const_loads = const_val.collect_constant_loads_in(code);
    if (m_skip_multiple_const_0 && const_val.is_int_kind() &&
        const_val.get_int_value() == 0 && !const_loads.empty()) {
      const_val_loads.emplace_back(const_val, const_loads.front());
      TRACE(METH_DEDUP, 9, "const value: skip const 0 loads\n");
      continue;
    }
    for (auto& load : const_loads) {
      if (matched_loads.count(load.first) > 0) {
        // If the same const load insn has been matched for multiple const
        // values in the @MethodMeta annotation, we skip it.
        // Trying to lift the same const load insn later will lead to a crash.
        continue;
      }
      const_val_loads.emplace_back(const_val, load);
      TRACE(METH_DEDUP,
            9,
            "const value: %s matched with const-load %s\n",
            const_val.to_str().c_str(),
            SHOW(load.first));
      matched_loads.insert(load.first);
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

/**
 * Will return a newly created method that loads the argument values and
 * passes them to the `callee` method.
 *
 *   <ret_type> <method_name>$redex(<method_args>...) {
 *      const-* <local_reg_0> field_1_data
 *      const-* <local_reg_1> field_2_data
 *      ...
 *      invoke-static <callee> <method_args ...> <local_regs ...>
 *   }
 */
DexMethod* ConstantValues::create_stub_method(DexMethod* callee) {
  DexType* type = callee->get_class();
  // Assuming that callee's proto is already modified by appending the lifted
  // params.
  auto appended_proto = callee->get_proto();
  auto stub_arg_list =
      type_reference::drop_and_make(appended_proto->get_args(), size());
  auto stub_proto =
      DexProto::make_proto(appended_proto->get_rtype(), stub_arg_list);
  auto name = DexString::make_string(callee->get_name()->str() + "$stub");
  name = DexMethod::get_unique_name(type, name, stub_proto);
  TRACE(METH_DEDUP, 9, "const value: stub name %s\n", name->c_str());
  auto mc = new MethodCreator(type,
                              name,
                              stub_proto,
                              callee->get_access(),
                              nullptr, // anno
                              false // with_debug_item
  );
  auto mb = mc->get_main_block();
  // Setup args for calling the callee.
  size_t arg_loc = 0;
  std::vector<Location> args;
  if (!is_static(callee)) {
    args.push_back(mc->get_local(arg_loc++));
  }
  for (size_t i = 0; i < stub_arg_list->size(); ++i) {
    args.push_back(mc->get_local(arg_loc++));
  }
  for (auto& cval : m_const_vals) {
    if (cval.is_invalid()) {
      continue;
    }
    auto loc = mc->make_local(cval.get_constant_type());
    if (cval.is_int_value()) {
      mb->load_const(loc, static_cast<int32_t>(cval.get_int_value()));
    } else {
      mb->load_const(loc, DexString::make_string(cval.get_str_value()));
    }
    args.push_back(loc);
  }
  mb->invoke(callee, args);

  DexType* ret_type = callee->get_proto()->get_rtype();
  if (ret_type == get_void_type()) {
    mb->ret_void();
  } else {
    auto ret_loc = mc->make_local(ret_type);
    mb->move_result(ret_loc, ret_type);
    mb->ret(ret_type, ret_loc);
  }

  auto stub = mc->create();
  // Propogate deobfuscated name
  auto orig_name = callee->get_deobfuscated_name();
  auto pos = orig_name.find(":");
  always_assert(pos != std::string::npos);
  auto new_name =
      orig_name.substr(0, pos) + "$stub" + ":" + show_deobfuscated(stub_proto);
  stub->set_deobfuscated_name(new_name);
  TRACE(METH_DEDUP, 9, "stub's new deobfuscated name %s\n", new_name.c_str());
  // Add stub to class
  type_class(type)->add_method(stub);
  TRACE(METH_DEDUP,
        9,
        "const value: created stub %s\n%s\n",
        SHOW(stub),
        SHOW(stub->get_code()));
  return stub;
}
