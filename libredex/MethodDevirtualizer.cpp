/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "MethodDevirtualizer.h"

#include "Mutators.h"
#include "Resolver.h"
#include "VirtualScope.h"
#include "Walkers.h"

namespace {

using CallSiteReferences =
    std::map<const DexMethod*, std::vector<IRInstruction*>>;

bool is_invoke_virtual(DexOpcode op) {
  return op == OPCODE_INVOKE_VIRTUAL || op == OPCODE_INVOKE_VIRTUAL_RANGE;
}

bool is_invoke_super(DexOpcode op) {
  return op == OPCODE_INVOKE_SUPER || op == OPCODE_INVOKE_SUPER_RANGE;
}

bool is_invoke_direct(DexOpcode op) {
  return op == OPCODE_INVOKE_DIRECT || op == OPCODE_INVOKE_DIRECT_RANGE;
}

bool is_invoke_static(DexOpcode op) {
  return op == OPCODE_INVOKE_STATIC || op == OPCODE_INVOKE_STATIC_RANGE;
}

DexOpcode invoke_virtual_to_static(DexOpcode op) {
  switch (op) {
  case OPCODE_INVOKE_VIRTUAL:
    return OPCODE_INVOKE_STATIC;
  case OPCODE_INVOKE_VIRTUAL_RANGE:
    return OPCODE_INVOKE_STATIC_RANGE;
  default:
    always_assert(false);
  }
}

DexOpcode invoke_super_to_static(DexOpcode op) {
  switch (op) {
  case OPCODE_INVOKE_SUPER:
    return OPCODE_INVOKE_STATIC;
  case OPCODE_INVOKE_SUPER_RANGE:
    return OPCODE_INVOKE_STATIC_RANGE;
  default:
    always_assert(false);
  }
}

DexOpcode invoke_direct_to_static(DexOpcode op) {
  switch (op) {
  case OPCODE_INVOKE_DIRECT:
    return OPCODE_INVOKE_STATIC;
  case OPCODE_INVOKE_DIRECT_RANGE:
    return OPCODE_INVOKE_STATIC_RANGE;
  default:
    always_assert(false);
  }
}

void patch_call_site(DexMethod* callee,
                     IRInstruction* method_inst,
                     DevirtualizerMetrics& metrics) {
  auto op = method_inst->opcode();
  if (is_invoke_virtual(op)) {
    method_inst->set_opcode(invoke_virtual_to_static(op));
    metrics.num_virtual_calls++;
  } else if (is_invoke_super(op)) {
    method_inst->set_opcode(invoke_super_to_static(op));
    metrics.num_super_calls++;
  } else if (is_invoke_direct(op)) {
    method_inst->set_opcode(invoke_direct_to_static(op));
    metrics.num_direct_calls++;
  } else {
    always_assert_log(false, SHOW(op));
  }

  method_inst->set_method(callee);
}

void fix_call_sites_and_drop_this_arg(
    const std::vector<DexClass*>& scope,
    const std::unordered_set<DexMethod*>& statics,
    DevirtualizerMetrics& metrics) {
  const auto fixer = [&](DexMethod*, IRCode& code) {
    std::vector<std::pair<IRInstruction*, IRInstruction*>> replacements;
    for (auto& mie : InstructionIterable(&code)) {
      auto inst = mie.insn;
      if (!inst->has_method()) {
        continue;
      }
      auto method = inst->get_method();
      if (!method->is_concrete()) {
        method = resolve_method(method, MethodSearch::Any);
      }
      if (!statics.count(method)) {
        continue;
      }
      patch_call_site(method, inst, metrics);
      if (is_invoke_range(inst->opcode())) {
        if (inst->range_size() == 1) {
          auto repl = new IRInstruction(OPCODE_INVOKE_STATIC);
          repl->set_method(method)->set_arg_word_count(0);
          replacements.emplace_back(inst, repl);
        } else {
          inst->set_range_base(inst->range_base() + 1);
          inst->set_range_size(inst->range_size() - 1);
        }
      } else {
        auto nargs = inst->arg_word_count();
        for (uint16_t i = 0; i < nargs - 1; i++) {
          inst->set_src(i, inst->src(i + 1));
        }
        inst->set_arg_word_count(nargs - 1);
      }
    }
    for (auto& pair : replacements) {
      code.replace_opcode(pair.first, pair.second);
    }
  };

  walk_code(scope, [](DexMethod*) { return true; }, fixer);
}

void fix_call_sites(const std::vector<DexClass*>& scope,
                    const std::unordered_set<DexMethod*>& target_methods,
                    DevirtualizerMetrics& metrics) {
  const auto fixer = [&](DexMethod*, IRInstruction* opcode) {
    if (!opcode->has_method()) {
      return;
    }

    auto method = opcode->get_method();
    if (!method->is_concrete()) {
      method = resolve_method(method, MethodSearch::Virtual);
    }
    if (!target_methods.count(method)) {
      return;
    }

    always_assert(!is_invoke_static(opcode->opcode()));
    patch_call_site(method, opcode, metrics);
  };

  walk_opcodes(scope, [](DexMethod*) { return true; }, fixer);
}

void make_methods_static(const std::unordered_set<DexMethod*>& methods,
                         bool keep_this) {
  for (auto method : methods) {
    TRACE(VIRT,
          2,
          "Staticized method: %s, keep this: %d\n",
          SHOW(method),
          keep_this);
    mutators::make_static(
        method, keep_this ? mutators::KeepThis::Yes : mutators::KeepThis::No);
  }
}

bool uses_this(const DexMethod* method) {
  auto const* code = method->get_code();
  always_assert(!is_static(method) && code != nullptr);

  auto const this_insn = InstructionIterable(code).begin()->insn;
  always_assert(this_insn->opcode() == IOPCODE_LOAD_PARAM_OBJECT);
  auto const this_reg = this_insn->dest();
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (opcode::has_range(insn->opcode())) {
      if (this_reg >= insn->range_base() &&
          this_reg < (insn->range_base() + insn->range_size())) {
        return true;
      }
    }
    for (unsigned i = 0; i < insn->srcs_size(); i++) {
      if (this_reg == insn->src(i)) {
        return true;
      }
    }
  }
  return false;
}

std::vector<DexMethod*> get_devirtualizable_vmethods(
    const std::vector<DexClass*>& scope,
    const std::vector<DexClass*>& targets) {
  std::vector<DexMethod*> ret;
  auto vmethods = devirtualize(scope);
  auto targets_set =
      std::unordered_set<DexClass*>(targets.begin(), targets.end());
  for (auto m : vmethods) {
    auto cls = type_class(m->get_class());
    if (targets_set.count(cls) > 0) {
      ret.push_back(m);
    }
  }
  return ret;
}

std::vector<DexMethod*> get_devirtualizable_vmethods(
    const std::vector<DexClass*>& scope,
    const std::vector<DexMethod*>& targets) {
  ClassHierarchy class_hierarchy = build_type_hierarchy(scope);
  auto signature_map = build_signature_map(class_hierarchy);

  std::vector<DexMethod*> res;
  for (const auto m : targets) {
    always_assert(!is_static(m) && !is_private(m) && !is_any_init(m));
    if (can_devirtualize(signature_map, m)) {
      res.push_back(m);
    }
  }

  return res;
}

std::vector<DexMethod*> get_devirtualizable_dmethods(
    const std::vector<DexClass*>& scope,
    const std::vector<DexClass*>& targets) {
  std::vector<DexMethod*> ret;
  auto targets_set =
      std::unordered_set<DexClass*>(targets.begin(), targets.end());
  for (auto cls : scope) {
    if (targets_set.count(cls) == 0) {
      continue;
    }
    for (auto m : cls->get_dmethods()) {
      if (is_any_init(m) || is_static(m)) {
        continue;
      }
      ret.push_back(m);
    }
  }
  return ret;
}

void verify_and_split(const std::vector<DexMethod*>& candidates,
                      std::unordered_set<DexMethod*>& using_this,
                      std::unordered_set<DexMethod*>& not_using_this) {
  for (const auto m : candidates) {
    if (keep(m) || m->is_external() || is_abstract(m)) {
      continue;
    }
    if (uses_this(m)) {
      using_this.insert(m);
    } else {
      not_using_this.insert(m);
    }
  }
}

} // namespace

void MethodDevirtualizer::staticize_methods_not_using_this(
    const std::vector<DexClass*>& scope,
    const std::unordered_set<DexMethod*>& methods) {
  fix_call_sites_and_drop_this_arg(scope, methods, m_metrics);
  make_methods_static(methods, false);
  TRACE(VIRT, 1, "Staticized %lu methods not using this\n", methods.size());
  m_metrics.num_methods_not_using_this += methods.size();
}

void MethodDevirtualizer::staticize_methods_using_this(
    const std::vector<DexClass*>& scope,
    const std::unordered_set<DexMethod*>& methods) {
  fix_call_sites(scope, methods, m_metrics);
  make_methods_static(methods, true);
  TRACE(VIRT, 1, "Staticized %lu methods using this\n", methods.size());
  m_metrics.num_methods_using_this += methods.size();
}

DevirtualizerMetrics MethodDevirtualizer::devirtualize_methods(
    const Scope& scope, const std::vector<DexClass*>& target_classes) {
  reset_metrics();
  auto vmethods = get_devirtualizable_vmethods(scope, target_classes);
  std::unordered_set<DexMethod*> using_this, not_using_this;
  verify_and_split(vmethods, using_this, not_using_this);
  TRACE(VIRT,
        2,
        " VIRT to devirt vmethods using this %lu, not using this %lu\n",
        using_this.size(),
        not_using_this.size());

  if (m_config.vmethods_not_using_this) {
    staticize_methods_not_using_this(scope, not_using_this);
  }

  if (m_config.vmethods_using_this) {
    staticize_methods_using_this(scope, using_this);
  }

  auto dmethods = get_devirtualizable_dmethods(scope, target_classes);
  using_this.clear();
  not_using_this.clear();
  verify_and_split(dmethods, using_this, not_using_this);
  TRACE(VIRT,
        2,
        " VIRT to devirt dmethods using this %lu, not using this %lu\n",
        using_this.size(),
        not_using_this.size());

  if (m_config.dmethods_not_using_this) {
    staticize_methods_not_using_this(scope, not_using_this);
  }

  if (m_config.dmethods_using_this) {
    staticize_methods_using_this(scope, using_this);
  }

  return m_metrics;
}

DevirtualizerMetrics MethodDevirtualizer::devirtualize_vmethods(
    const Scope& scope, const std::vector<DexMethod*>& methods) {
  reset_metrics();
  const auto candidates = get_devirtualizable_vmethods(scope, methods);
  std::unordered_set<DexMethod*> using_this, not_using_this;
  verify_and_split(candidates, using_this, not_using_this);

  if (m_config.vmethods_using_this) {
    staticize_methods_using_this(scope, using_this);
  }

  if (m_config.vmethods_not_using_this) {
    staticize_methods_not_using_this(scope, not_using_this);
  }

  return m_metrics;
}
