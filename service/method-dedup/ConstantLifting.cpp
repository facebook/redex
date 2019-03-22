/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantLifting.h"

#include "AnnoUtils.h"
#include "ConstantValue.h"
#include "IRCode.h"
#include "MethodReference.h"
#include "Resolver.h"
#include "TypeReference.h"
#include "TypeTags.h"

using MethodOrderedSet = std::set<DexMethod*, dexmethods_comparator>;

namespace {

constexpr const char* METHOD_META =
    "Lcom/facebook/redex/annotations/MethodMeta;";
constexpr const char* CONST_TYPE_ANNO_ATTR_NAME = "constantTypes";
constexpr const char* CONST_VALUE_ANNO_ATTR_NAME = "constantValues";

bool overlaps_with_an_existing_virtual_scope(DexType* type,
                                             DexString* name,
                                             DexProto* proto) {
  if (DexMethod::get_method(type, name, proto)) {
    return true;
  }
  DexClass* cls = type_class(type);
  while (cls->get_super_class()) {
    type = cls->get_super_class();
    if (DexMethod::get_method(type, name, proto)) {
      return true;
    }
    cls = type_class(type);
  }

  return false;
}

} // namespace

using MethodToConstants =
    std::map<DexMethod*, ConstantValues, dexmethods_comparator>;

const DexType* s_method_meta_anno;

ConstantLifting::ConstantLifting() : m_num_const_lifted_methods(0) {
  s_method_meta_anno = DexType::get_type(DexString::get_string(METHOD_META));
}

bool ConstantLifting::is_applicable_to_constant_lifting(
    const DexMethod* method) {
  if (is_synthetic(method) || !has_anno(method, s_method_meta_anno)) {
    return false;
  }
  if (!has_attribute(method, s_method_meta_anno, CONST_TYPE_ANNO_ATTR_NAME)) {
    return false;
  }
  return true;
}

std::vector<DexMethod*> ConstantLifting::lift_constants_from(
    const Scope& scope,
    const TypeTags* type_tags,
    const std::vector<DexMethod*>& methods,
    const size_t stud_method_threshold) {
  MethodOrderedSet lifted;
  MethodToConstants lifted_constants;
  for (auto method : methods) {
    always_assert(has_anno(method, s_method_meta_anno));
    auto kinds_str = parse_str_anno_value(method, s_method_meta_anno,
                                          CONST_TYPE_ANNO_ATTR_NAME);
    auto vals_str = parse_str_anno_value(method, s_method_meta_anno,
                                         CONST_VALUE_ANNO_ATTR_NAME);

    ConstantValues const_vals(type_tags,
                              kinds_str,
                              vals_str,
                              stud_method_threshold,
                              method->get_code());
    auto const_loads = const_vals.collect_constant_loads(method->get_code());
    if (const_loads.size() == 0) {
      // No matching constant found.
      TRACE(METH_DEDUP,
            5,
            "  no matching constant %s found in %s\n",
            const_vals.to_str().c_str(),
            SHOW(method));
      TRACE(METH_DEDUP, 9, "%s\n", SHOW(method->get_code()));
      continue;
    }
    lifted.insert(method);
    lifted_constants.emplace(method, const_vals);
    TRACE(METH_DEDUP,
          5,
          "constant lifting: const value %s\n",
          const_vals.to_str().c_str());
    TRACE(METH_DEDUP, 9, "    in %s\n", SHOW(method));

    // Add constant to arg list.
    auto old_proto = method->get_proto();
    auto const_types = const_vals.get_constant_types();
    auto arg_list =
        type_reference::append_and_make(old_proto->get_args(), const_types);
    auto new_proto = DexProto::make_proto(old_proto->get_rtype(), arg_list);

    // Find a non-conflicting name
    auto name = method->get_name();
    std::string suffix = "$r";
    while (overlaps_with_an_existing_virtual_scope(method->get_class(), name,
                                                   new_proto)) {
      name = DexString::make_string(name->c_str() + suffix);
      TRACE(METH_DEDUP,
            9,
            "constant lifting method name updated to %s\n",
            name->c_str());
    }

    // Update method
    DexMethodSpec spec;
    spec.name = name;
    spec.proto = new_proto;
    method->change(spec,
                   true /* rename on collision */,
                   true /* update deobfuscated name */);

    // Insert param load.
    auto code = method->get_code();
    auto params = code->get_param_instructions();
    for (const auto& const_val : const_vals.get_constant_values()) {
      if (const_val.is_invalid()) {
        continue;
      }
      auto load_type_tag_param =
          const_val.is_int_value()
              ? new IRInstruction(IOPCODE_LOAD_PARAM)
              : new IRInstruction(IOPCODE_LOAD_PARAM_OBJECT);
      load_type_tag_param->set_dest(const_val.get_param_reg());
      code->insert_before(params.end(), load_type_tag_param);
    }

    // Replace const loads with moves.
    for (const auto& load : const_loads) {
      auto const_val = load.first;
      auto insn = load.second.first;
      auto dest = load.second.second;
      auto move_const_arg = const_val.is_int_value()
                                ? new IRInstruction(OPCODE_MOVE)
                                : new IRInstruction(OPCODE_MOVE_OBJECT);
      move_const_arg->set_dest(dest);
      move_const_arg->set_src(0, const_val.get_param_reg());
      code->replace_opcode(insn, move_const_arg);
    }
  }
  TRACE(METH_DEDUP,
        5,
        "constant lifting applied to %ld among %ld\n",
        lifted.size(),
        methods.size());
  m_num_const_lifted_methods += lifted.size();

  // Update call sites
  std::vector<DexMethod*> stub_methods;
  auto call_sites = method_reference::collect_call_refs(scope, lifted);
  for (const auto& callsite : call_sites) {
    auto meth = callsite.caller;
    auto insn = callsite.mie->insn;
    const auto callee =
        resolve_method(insn->get_method(), opcode_to_search(insn));
    always_assert(callee != nullptr);
    auto const_vals = lifted_constants.at(callee);
    auto code = meth->get_code();
    if (const_vals.needs_stub()) {
      // Insert const load
      std::vector<uint16_t> args;
      for (size_t i = 0; i < insn->srcs_size(); i++) {
        args.push_back(insn->src(i));
      }
      auto stub = const_vals.create_stub_method(callee);
      auto invoke = method_reference::make_invoke(stub, insn->opcode(), args);
      code->insert_after(insn, {invoke});
      // remove original call.
      code->remove_opcode(insn);
      stub_methods.push_back(stub);
    } else {
      // Make const load
      std::vector<uint16_t> const_regs;
      for (size_t i = 0; i < const_vals.size(); ++i) {
        const_regs.push_back(code->allocate_temp());
      }
      auto const_loads_and_invoke = const_vals.make_const_loads(const_regs);
      // Insert const load
      std::vector<uint16_t> args;
      for (size_t i = 0; i < insn->srcs_size(); i++) {
        args.push_back(insn->src(i));
      }
      args.insert(args.end(), const_regs.begin(), const_regs.end());
      auto invoke = method_reference::make_invoke(callee, insn->opcode(), args);
      const_loads_and_invoke.push_back(invoke);
      code->insert_after(insn, const_loads_and_invoke);
      // remove original call.
      code->remove_opcode(insn);
    }
    TRACE(METH_DEDUP,
          9,
          " patched call site in %s\n%s\n",
          SHOW(meth),
          SHOW(code));
  }

  return stub_methods;
}
