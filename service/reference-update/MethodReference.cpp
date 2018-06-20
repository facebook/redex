/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "MethodReference.h"

#include "Resolver.h"
#include "Walkers.h"

namespace method_reference {

IRInstruction* make_load_const(uint16_t dest, size_t val) {
  auto load = new IRInstruction(OPCODE_CONST);
  load->set_dest(dest);
  load->set_literal(static_cast<int32_t>(val));
  return load;
}

IRInstruction* make_invoke(DexMethod* callee,
                           IROpcode opcode,
                           std::vector<uint16_t> args) {
  always_assert(callee->is_def() && is_public(callee));
  auto invoke = (new IRInstruction(opcode))->set_method(callee);
  invoke->set_arg_word_count(args.size());
  for (size_t i = 0; i < args.size(); i++) {
    invoke->set_src(i, args.at(i));
  }
  return invoke;
}

void patch_callsite(const CallSiteSpec& spec,
                    const boost::optional<uint32_t>& additional_arg) {
  const auto caller = spec.caller;
  const auto call_insn = spec.call_insn;
  const auto callee = spec.new_callee;
  TRACE(REFU, 9, " patching call site at %s\n", SHOW(call_insn));

  // Update package protected or protected methods to public.
  if (is_static(callee) || is_any_init(callee) || !is_private(callee)) {
    set_public(callee);
  }
  always_assert_log(
      is_public(callee) || callee->get_class() == caller->get_class(),
      "Updating a call site of %s when not accessible from %s\n",
      SHOW(callee), SHOW(caller));

  auto code = caller->get_code();
  auto additional_arg_reg = code->allocate_temp();
  auto load_additional_arg =
      make_load_const(additional_arg_reg, additional_arg.get_value_or(0));

  // Assuming the following move-result is there and good.
  std::vector<uint16_t> args;
  for (size_t i = 0; i < call_insn->srcs_size(); i++) {
    args.push_back(call_insn->src(i));
  }
  if (additional_arg != boost::none) {
    args.push_back(additional_arg_reg);
  }
  auto invoke = make_invoke(callee, call_insn->opcode(), args);
  if (additional_arg != boost::none) {
    code->insert_after(
        call_insn, std::vector<IRInstruction*>{load_additional_arg, invoke});
  } else {
    code->insert_after(call_insn, std::vector<IRInstruction*>{invoke});
  }

  // remove original call.
  code->remove_opcode(call_insn);
  TRACE(REFU, 9, " patched call site in %s\n%s\n", SHOW(caller), SHOW(code));
}

void update_call_refs_simple(
    const Scope& scope,
    const std::unordered_map<DexMethod*, DexMethod*>& old_to_new_callee) {
  auto patcher = [&](DexMethod* meth, IRCode& code) {
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (!insn->has_method()) {
        continue;
      }
      const auto method =
          resolve_method(insn->get_method(), opcode_to_search(insn));
      if (method == nullptr || old_to_new_callee.count(method) == 0) {
        continue;
      }
      auto new_callee = old_to_new_callee.at(method);
      // At this point, a non static private should not exist.
      always_assert_log(!is_private(new_callee) || is_static(new_callee),
                        "%s\n",
                        vshow(new_callee).c_str());
      TRACE(REFU, 9, " Updated call %s to %s\n", SHOW(insn), SHOW(new_callee));
      insn->set_method(new_callee);
      if (new_callee->is_virtual()) {
        always_assert_log(is_invoke_virtual(insn->opcode()),
                          "invalid callsite %s\n",
                          SHOW(insn));
      } else if (is_static(new_callee)) {
        always_assert_log(is_invoke_static(insn->opcode()),
                          "invalid callsite %s\n",
                          SHOW(insn));
      }
    }
  };
  walk::parallel::code(scope, patcher);
}

CallSites collect_call_refs(const Scope& scope,
                            const MethodOrderedSet& callees) {
  auto patcher = [&](std::nullptr_t, DexMethod* meth) {
    CallSites call_sites;
    auto code = meth->get_code();
    if (!code) {
      return call_sites;
    }

    for (auto& mie : InstructionIterable(meth->get_code())) {
      auto insn = mie.insn;
      if (!insn->has_method()) {
        continue;
      }

      const auto method =
          resolve_method(insn->get_method(),
                         opcode_to_search(const_cast<IRInstruction*>(insn)));
      if (method == nullptr || callees.count(method) == 0) {
        continue;
      }

      call_sites.emplace_back(meth, insn);
      TRACE(REFU, 9, "  Found call %s from %s\n", SHOW(insn), SHOW(meth));
    }

    return call_sites;
  };

  CallSites call_sites =
      walk::parallel::reduce_methods<std::nullptr_t, CallSites>(
          scope,
          patcher,
          [](CallSites left, CallSites right) {
            left.insert(left.end(), right.begin(), right.end());
            return left;
          },
          [](int) { return nullptr; });
  return call_sites;
}

} // namespace method_reference
