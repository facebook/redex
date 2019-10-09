/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodDevirtualizer.h"

#include "Mutators.h"
#include "Resolver.h"
#include "VirtualScope.h"
#include "Walkers.h"

namespace {

using CallSiteReferences =
    std::map<const DexMethod*, std::vector<IRInstruction*>>;

struct CallCounter {
  uint32_t virtuals{0};
  uint32_t supers{0};
  uint32_t directs{0};

  static CallCounter plus(CallCounter a, CallCounter b) {
    a.virtuals += b.virtuals;
    a.supers += b.supers;
    a.directs += b.directs;
    return a;
  }
};

void patch_call_site(DexMethod* callee,
                     IRInstruction* method_inst,
                     CallCounter& counter) {
  auto op = method_inst->opcode();
  if (is_invoke_virtual(op)) {
    method_inst->set_opcode(OPCODE_INVOKE_STATIC);
    counter.virtuals++;
  } else if (is_invoke_super(op)) {
    method_inst->set_opcode(OPCODE_INVOKE_STATIC);
    counter.supers++;
  } else if (is_invoke_direct(op)) {
    method_inst->set_opcode(OPCODE_INVOKE_STATIC);
    counter.directs++;
  } else {
    always_assert_log(false, SHOW(op));
  }

  method_inst->set_method(callee);
}

void fix_call_sites(const std::vector<DexClass*>& scope,
                    const std::unordered_set<DexMethod*>& target_methods,
                    DevirtualizerMetrics& metrics,
                    bool drop_this = false) {
  const auto fixer = [&target_methods, drop_this](DexMethod* m) -> CallCounter {
    CallCounter call_counter;
    IRCode* code = m->get_code();
    if (code == nullptr) {
      return call_counter;
    }

    for (const MethodItemEntry& mie : InstructionIterable(code)) {
      IRInstruction* insn = mie.insn;
      if (!insn->has_method()) {
        continue;
      }

      MethodSearch type = drop_this ? MethodSearch::Any : MethodSearch::Virtual;
      auto method = resolve_method(insn->get_method(), type);
      if (method == nullptr || !target_methods.count(method)) {
        continue;
      }

      always_assert(drop_this || !is_invoke_static(insn->opcode()));
      patch_call_site(method, insn, call_counter);

      if (drop_this) {
        auto nargs = insn->arg_word_count();
        for (uint16_t i = 0; i < nargs - 1; i++) {
          insn->set_src(i, insn->src(i + 1));
        }
        insn->set_arg_word_count(nargs - 1);
      }
    }

    return call_counter;
  };

  CallCounter call_counter =
      walk::parallel::reduce_methods<CallCounter, Scope>(
          scope,
          fixer,
          [](CallCounter a, CallCounter b) -> CallCounter {
            a.virtuals += b.virtuals;
            a.supers += b.supers;
            a.directs += b.directs;
            return a;
          });

  metrics.num_virtual_calls += call_counter.virtuals;
  metrics.num_super_calls += call_counter.supers;
  metrics.num_direct_calls += call_counter.directs;
}

void make_methods_static(const std::unordered_set<DexMethod*>& methods,
                         bool keep_this) {
  // TODO" Change callers to pass this as a sorted vector or an ordered set.
  std::vector<DexMethod*> meth_list(methods.begin(), methods.end());
  std::sort(meth_list.begin(), meth_list.end(), compare_dexmethods);

  for (auto* method : meth_list) {
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
  always_assert_log(!is_static(method) && code != nullptr, "%s", SHOW(method));

  auto iterable = InstructionIterable(code);
  auto const this_insn = iterable.begin()->insn;
  always_assert(this_insn->opcode() == IOPCODE_LOAD_PARAM_OBJECT);
  auto const this_reg = this_insn->dest();
  for (const auto& mie : iterable) {
    auto insn = mie.insn;
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

} // namespace

void MethodDevirtualizer::verify_and_split(
    const std::vector<DexMethod*>& candidates,
    std::unordered_set<DexMethod*>& using_this,
    std::unordered_set<DexMethod*>& not_using_this) {
  for (const auto m : candidates) {
    if (!m_config.ignore_keep && has_keep(m)) {
      TRACE(VIRT, 2, "failed to devirt method %s: keep\n", SHOW(m));
      continue;
    }
    if (m->is_external() || is_abstract(m) || is_native(m)) {
      TRACE(VIRT,
            2,
            "failed to devirt method %s: external %d, abstract %d, native %d\n",
            SHOW(m),
            m->is_external(),
            is_abstract(m),
            is_native(m));
      continue;
    }
    if (uses_this(m)) {
      using_this.insert(m);
    } else {
      not_using_this.insert(m);
    }
  }
}

void MethodDevirtualizer::staticize_methods_not_using_this(
    const std::vector<DexClass*>& scope,
    const std::unordered_set<DexMethod*>& methods) {
  fix_call_sites(scope, methods, m_metrics, true /* drop_this */);
  make_methods_static(methods, false);
  TRACE(VIRT, 1, "Staticized %lu methods not using this\n", methods.size());
  m_metrics.num_methods_not_using_this += methods.size();
}

void MethodDevirtualizer::staticize_methods_using_this(
    const std::vector<DexClass*>& scope,
    const std::unordered_set<DexMethod*>& methods) {
  fix_call_sites(scope, methods, m_metrics, false /* drop_this */);
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
