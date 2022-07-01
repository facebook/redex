/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodReference.h"

#include "IRList.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {
Scope build_class_scope_excluding_primary_dex(const DexStoresVector& stores) {
  Scope result;
  for (const auto& store : stores) {
    const auto& dexen = store.get_dexen();
    auto it = dexen.begin();
    if (store.is_root_store()) {
      it++;
    }
    for (; it != dexen.end(); it++) {
      for (auto& clazz : *it) {
        result.push_back(clazz);
      }
    }
  }
  return result;
}
} // namespace

namespace method_reference {

IRInstruction* make_load_const(reg_t dest, size_t val) {
  auto load = new IRInstruction(OPCODE_CONST);
  load->set_dest(dest);
  load->set_literal(static_cast<int32_t>(val));
  return load;
}

IRInstruction* make_invoke(DexMethod* callee,
                           IROpcode opcode,
                           std::vector<reg_t> args) {
  always_assert(callee->is_def() && is_public(callee));
  auto invoke = (new IRInstruction(opcode))->set_method(callee);
  invoke->set_srcs_size(args.size());
  for (size_t i = 0; i < args.size(); i++) {
    invoke->set_src(i, args.at(i));
  }
  return invoke;
}

void patch_callsite(const CallSite& callsite, const NewCallee& new_callee) {
  if (is_static(new_callee.method) || method::is_any_init(new_callee.method) ||
      new_callee.method->is_virtual()) {
    set_public(new_callee.method);
  }
  always_assert_log(is_public(new_callee.method) ||
                        new_callee.method->get_class() ==
                            callsite.caller->get_class(),
                    "\tUpdating a callsite of %s when not accessible from %s\n",
                    SHOW(new_callee.method), SHOW(callsite.caller));

  auto code = callsite.caller->get_code();
  auto iterator = code->iterator_to(*callsite.mie);
  auto insn = callsite.mie->insn;
  if (new_callee.additional_args != boost::none) {
    const auto& args = new_callee.additional_args.get();
    auto old_size = insn->srcs_size();
    insn->set_srcs_size(old_size + args.size());
    size_t pos = old_size;
    for (uint32_t arg : args) {
      auto reg = code->allocate_temp();
      // Seems it is different from dasm(OPCODE_CONST, {{VREG, reg}, {LITERAL,
      // arg}}) which will cause instruction_lowering crash. Why?
      auto load_const = make_load_const(reg, arg);
      code->insert_before(iterator, load_const);
      insn->set_src(pos++, reg);
    }
  }
  insn->set_method(new_callee.method);
  // Assuming the following move-result is there and good.
}

void update_call_refs_simple(
    const Scope& scope,
    const std::unordered_map<DexMethod*, DexMethod*>& old_to_new_callee) {
  if (old_to_new_callee.empty()) {
    return;
  }

  auto patcher = [&](DexMethod* meth, IRCode& code) {
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (!insn->has_method()) {
        continue;
      }
      const auto method =
          resolve_method(insn->get_method(), opcode_to_search(insn), meth);
      if (method == nullptr || old_to_new_callee.count(method) == 0) {
        continue;
      }
      auto new_callee = old_to_new_callee.at(method);
      // At this point, a non static private should not exist.
      always_assert_log(!is_private(new_callee) || is_static(new_callee),
                        "%s\n",
                        vshow(new_callee).c_str());
      TRACE(REFU, 9, " Updated call %s to %s", SHOW(insn), SHOW(new_callee));
      insn->set_method(new_callee);
      if (new_callee->is_virtual()) {
        always_assert_log(opcode::is_invoke_virtual(insn->opcode()),
                          "invalid callsite %s\n",
                          SHOW(insn));
      } else if (is_static(new_callee)) {
        always_assert_log(opcode::is_invoke_static(insn->opcode()),
                          "invalid callsite %s\n",
                          SHOW(insn));
      }
    }
  };
  walk::parallel::code(scope, patcher);
}

template <typename T>
CallSites collect_call_refs(const Scope& scope, const T& callees) {
  if (callees.empty()) {
    CallSites empty;
    return empty;
  }
  auto patcher = [&](DexMethod* caller) {
    CallSites call_sites;
    auto code = caller->get_code();
    if (!code) {
      return call_sites;
    }

    for (auto& mie : InstructionIterable(caller->get_code())) {
      auto insn = mie.insn;
      if (!insn->has_method()) {
        continue;
      }

      const auto callee = resolve_method(
          insn->get_method(),
          opcode_to_search(const_cast<IRInstruction*>(insn)), caller);
      if (callee == nullptr || callees.count(callee) == 0) {
        continue;
      }

      call_sites.emplace_back(caller, &mie, callee);
      TRACE(REFU, 9, "  Found call %s from %s", SHOW(insn), SHOW(caller));
    }

    return call_sites;
  };

  struct Append {
    void operator()(const CallSites& addend, CallSites* accumulator) {
      accumulator->insert(accumulator->end(), addend.begin(), addend.end());
    }
  };

  CallSites call_sites =
      walk::parallel::methods<CallSites, Append>(scope, patcher);
  return call_sites;
}

using MethodOrderedSet = std::set<DexMethod*, dexmethods_comparator>;
template CallSites collect_call_refs<MethodOrderedSet>(
    const Scope& scope, const MethodOrderedSet& callees);
template CallSites collect_call_refs<std::unordered_set<DexMethod*>>(
    const Scope& scope, const std::unordered_set<DexMethod*>& callees);

int wrap_instance_call_with_static(
    DexStoresVector& stores,
    const std::unordered_map<DexMethod*, DexMethod*>& methods_replacement,
    bool exclude_primary_dex) {
  Scope classes;
  if (!exclude_primary_dex) {
    classes = build_class_scope(stores);
  } else {
    classes = build_class_scope_excluding_primary_dex(stores);
  }
  std::unordered_set<DexType*> excluded_types;
  for (const auto& pair : methods_replacement) {
    always_assert(!is_static(pair.first));
    always_assert(is_static(pair.second));
    excluded_types.insert(pair.second->get_class());
  }
  std::atomic<uint32_t> total(0);
  // The excluded types are supposed to be wrapper and the only callers of the
  // original methods.
  walk::parallel::methods(classes, [&](DexMethod* method) {
    if (excluded_types.count(method->get_class())) {
      return;
    }
    auto code = method->get_code();
    if (code) {
      for (auto& mie : InstructionIterable(code)) {
        IRInstruction* insn = mie.insn;
        if (insn->opcode() != OPCODE_INVOKE_VIRTUAL) {
          continue;
        }
        auto method_ref = insn->get_method();
        auto it = methods_replacement.find(static_cast<DexMethod*>(method_ref));
        if (it != methods_replacement.end()) {
          always_assert(is_static(it->second));
          insn->set_opcode(OPCODE_INVOKE_STATIC);
          insn->set_method(it->second);
          ++total;
        }
      }
    }
  });
  return total;
}

} // namespace method_reference
