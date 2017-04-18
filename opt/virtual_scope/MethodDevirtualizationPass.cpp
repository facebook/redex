/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "MethodDevirtualizationPass.h"

#include "DexClass.h"
#include "DexUtil.h"
#include "Mutators.h"
#include "Resolver.h"
#include "VirtualScope.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_STATICIZED_METHODS_DROP_THIS = "num_staticized_methods_drop_this";
constexpr const char* METRIC_STATICIZED_METHODS_KEEP_THIS = "num_staticized_methods_keep_this";
constexpr const char* METRIC_VIRTUAL_CALLS_CONVERTED = "num_virtual_calls_converted";
constexpr const char* METRIC_DIRECT_CALLS_CONVERTED = "num_direct_calls_converted";
constexpr const char* METRIC_SUPER_CALLS_CONVERTED = "num_super_calls_converted";

using CallSiteReferences = std::map<const DexMethod*, std::vector<IRMethodInstruction*>>;

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

void patch_call_site(
  DexMethod* callee,
  IRMethodInstruction* method_inst,
  CallSiteMetrics& metrics
) {
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

  method_inst->rewrite_method(callee);
}

void fix_call_sites_and_drop_this_arg(
  const std::vector<DexClass*>& scope,
  const std::unordered_set<DexMethod*>& statics,
  CallSiteMetrics& metrics
) {
  const auto fixer =
    [&](DexMethod* /* unused */, IRCode& code) {
      std::vector<std::pair<IRInstruction*, IRInstruction*>> replacements;
      for (auto& mie : InstructionIterable(&code)) {
        auto inst = mie.insn;
        if (!inst->has_methods()) {
          continue;
        }
        auto mi = static_cast<IRMethodInstruction*>(inst);
        auto method = mi->get_method();
        if (!method->is_concrete()) {
          method = resolve_method(method, MethodSearch::Any);
        }
        if (!statics.count(method)) {
          continue;
        }
        patch_call_site(method, mi, metrics);
        if (is_invoke_range(inst->opcode())) {
          if (mi->range_size() == 1) {
            auto repl = new IRMethodInstruction(OPCODE_INVOKE_STATIC, method);
            repl->set_arg_word_count(0);
            replacements.emplace_back(mi, repl);
          } else {
            mi->set_range_base(mi->range_base() + 1);
            mi->set_range_size(mi->range_size() - 1);
          }
        } else {
          auto nargs = mi->arg_word_count();
          for (uint16_t i = 0; i < nargs - 1; i++) {
            mi->set_src(i, mi->src(i + 1));
          }
          mi->set_arg_word_count(nargs - 1);
        }
      }
      for (auto& pair : replacements) {
        code.replace_opcode(pair.first, pair.second);
      }
    };

  walk_code(scope, [](DexMethod*) { return true; }, fixer);
}

void fix_call_sites(
  const std::vector<DexClass*>& scope,
  const std::unordered_set<DexMethod*>& target_methods,
  CallSiteMetrics& metrics
) {
  const auto fixer =
    [&](DexMethod* /* unused */, IRInstruction* opcode) {
      if (!opcode->has_methods()) {
        return;
      }

      auto mop = static_cast<IRMethodInstruction*>(opcode);
      auto method = mop->get_method();
      if (!method->is_concrete()) {
        method = resolve_method(method, MethodSearch::Virtual);
      }
      if (!target_methods.count(method)) {
        return;
      }

      always_assert(!is_invoke_static(opcode->opcode()));
      patch_call_site(method, mop, metrics);
    };

  walk_opcodes(scope, [](DexMethod* /* unused */) { return true; }, fixer);
}

void make_methods_static(const std::unordered_set<DexMethod*>& methods, bool keep_this) {
  for (auto method : methods) {
    TRACE(VIRT, 2, "Staticized method: %s, keep this: %d\n", SHOW(method), keep_this);
    mutators::make_static(method, keep_this ? mutators::KeepThis::Yes : mutators::KeepThis::No);
  }
}

bool uses_this(const DexMethod* method) {
  auto const* code = method->get_code();
  always_assert(code != nullptr);

  auto const this_reg = code->get_registers_size() - code->get_ins_size();
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

std::vector<DexMethod*> devirtualizable_dmethods(const std::vector<DexClass*>& scope) {
  std::vector<DexMethod*> ret;
  for (auto cls : scope) {
    for (auto m : cls->get_dmethods()) {
      if (is_any_init(m) || is_static(m)) {
        continue;
      }
      ret.push_back(m);
    }
  }
  return ret;
}

std::unordered_set<DexMethod*> devirtualizable_methods(
  const std::vector<DexMethod*>& candidates
) {
  std::unordered_set<DexMethod*> found;
  for (auto const& method : candidates) {
    if (keep(method) ||
        method->is_external() ||
        is_abstract(method)) {
      continue;
    }
    found.emplace(method);
  }
  return found;
}

std::unordered_set<DexMethod*> devirtualizable_methods_not_using_this(
  const std::vector<DexMethod*>& candidates
) {
  std::unordered_set<DexMethod*> found;
  for (auto const& method : devirtualizable_methods(candidates)) {
    if (uses_this(method)) {
      continue;
    }
    found.emplace(method);
  }
  return found;
}

}

void MethodDevirtualizationPass::staticize_methods_not_using_this(
  const std::vector<DexClass*>& scope,
  PassManager& manager,
  const std::unordered_set<DexMethod*>& methods
) {
  fix_call_sites_and_drop_this_arg(scope, methods, m_call_site_metrics);
  make_methods_static(methods, false);
  manager.incr_metric(METRIC_STATICIZED_METHODS_DROP_THIS, methods.size());
  TRACE(VIRT, 1, "Staticized %lu methods not using this\n", methods.size());
}

void MethodDevirtualizationPass::staticize_methods_using_this(
  const std::vector<DexClass*>& scope,
  PassManager& manager,
  const std::unordered_set<DexMethod*>& methods
) {
  fix_call_sites(scope, methods, m_call_site_metrics);
  make_methods_static(methods, true);
  manager.incr_metric(METRIC_STATICIZED_METHODS_KEEP_THIS, methods.size());
  TRACE(VIRT, 1, "Staticized %lu methods using this\n", methods.size());
}

void MethodDevirtualizationPass::run_pass(
  DexStoresVector& stores,
  ConfigFiles& /* unused */,
  PassManager& manager
) {
  if (m_staticize_vmethods_not_using_this) {
    auto scope = build_class_scope(stores);
    auto candidates = devirtualize(scope);
    auto vmethods = devirtualizable_methods_not_using_this(candidates);
    staticize_methods_not_using_this(scope, manager, vmethods);
  }

  if (m_staticize_dmethods_not_using_this) {
    auto scope = build_class_scope(stores);
    auto candidates = devirtualizable_dmethods(scope);
    auto dmethods = devirtualizable_methods_not_using_this(candidates);
    staticize_methods_not_using_this(scope, manager, dmethods);
  }

  if (m_staticize_vmethods_using_this) {
    auto scope = build_class_scope(stores);
    const auto candidates = devirtualize(scope);
    const auto vmethods = devirtualizable_methods(candidates);
    staticize_methods_using_this(scope, manager, vmethods);
  }

  if (m_staticize_vmethods_using_this) {
    auto scope = build_class_scope(stores);
    auto candidates = devirtualizable_dmethods(scope);
    const auto vmethods = devirtualizable_methods(candidates);
    staticize_methods_using_this(scope, manager, vmethods);
  }

  manager.incr_metric(METRIC_VIRTUAL_CALLS_CONVERTED, m_call_site_metrics.num_virtual_calls);
  manager.incr_metric(METRIC_DIRECT_CALLS_CONVERTED, m_call_site_metrics.num_direct_calls);
  manager.incr_metric(METRIC_SUPER_CALLS_CONVERTED, m_call_site_metrics.num_super_calls);
}

static MethodDevirtualizationPass s_pass;
