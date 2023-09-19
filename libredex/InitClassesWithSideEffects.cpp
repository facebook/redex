/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InitClassesWithSideEffects.h"

#include "MethodUtil.h"
#include "Timer.h"
#include "Walkers.h"
#include <memory>

namespace init_classes {

const InitClasses* InitClassesWithSideEffects::compute(
    const DexClass* cls,
    const method::ClInitHasNoSideEffectsPredicate& clinit_has_no_side_effects,
    const std::unordered_set<DexMethod*>* non_true_virtuals) {
  const DexType* key = cls->get_type();
  const auto* cached = m_init_classes.get(key);
  if (cached) {
    return cached;
  }

  InitClasses classes;
  const auto* refined_cls = method::clinit_may_have_side_effects(
      cls, /* allow_benign_method_invocations */ true,
      &clinit_has_no_side_effects, non_true_virtuals);
  if (refined_cls == nullptr) {
  } else if (refined_cls != cls) {
    classes =
        *compute(refined_cls, clinit_has_no_side_effects, non_true_virtuals);
  } else {
    classes.push_back(cls);
    auto super_cls = type_class(cls->get_super_class());
    if (super_cls) {
      const auto super_classes =
          compute(super_cls, clinit_has_no_side_effects, non_true_virtuals);
      classes.insert(classes.end(), super_classes->begin(),
                     super_classes->end());
    }
  }
  auto [ptr, emplaced] =
      m_init_classes.get_or_emplace_and_assert_equal(key, std::move(classes));
  if (emplaced && ptr->empty()) {
    m_trivial_init_classes++;
  }
  return ptr;
}

InitClassesWithSideEffects::InitClassesWithSideEffects(
    const Scope& scope,
    bool create_init_class_insns,
    const method_override_graph::Graph* method_override_graph)
    : m_create_init_class_insns(create_init_class_insns) {
  Timer t("InitClassesWithSideEffects");
  std::unique_ptr<std::unordered_set<DexMethod*>> non_true_virtuals;
  if (method_override_graph) {
    non_true_virtuals = std::make_unique<std::unordered_set<DexMethod*>>(
        method_override_graph::get_non_true_virtuals(*method_override_graph,
                                                     scope));
  }
  size_t prev_trivial_init_classes;
  do {
    auto prev_init_classes = std::move(m_init_classes);
    prev_trivial_init_classes = m_trivial_init_classes.exchange(0);
    method::ClInitHasNoSideEffectsPredicate clinit_has_no_side_effects =
        [&](const DexType* type) {
          auto it = prev_init_classes.find(type);
          if (it != prev_init_classes.end()) {
            return it->second.empty();
          }
          auto cls = type_class(type);
          return cls && (cls->is_external() ||
                         cls->rstate.clinit_has_no_side_effects());
        };
    ConcurrentSet<DexClass*> added_clinit_has_no_side_effects;
    walk::parallel::classes(scope, [&](DexClass* cls) {
      if (compute(cls, clinit_has_no_side_effects, non_true_virtuals.get())
              ->empty() &&
          !cls->rstate.clinit_has_no_side_effects()) {
        added_clinit_has_no_side_effects.insert(cls);
      }
    });
    for (auto cls : added_clinit_has_no_side_effects) {
      cls->rstate.set_clinit_has_no_side_effects();
    }
    TRACE(ICL, 2,
          "InitClassesWithSideEffects: %zu trivial init classes, %zu "
          "clinit_has_no_side_effects added",
          (size_t)prev_trivial_init_classes,
          added_clinit_has_no_side_effects.size());
  } while (m_trivial_init_classes > prev_trivial_init_classes);
}

const InitClasses* InitClassesWithSideEffects::get(const DexType* type) const {
  auto it = m_init_classes.find(type);
  return it == m_init_classes.end() ? &m_empty_init_classes : &it->second;
}

const DexType* InitClassesWithSideEffects::refine(const DexType* type) const {
  auto init_classes = get(type);
  return init_classes->empty() ? nullptr : init_classes->front()->get_type();
}

IRInstruction* InitClassesWithSideEffects::create_init_class_insn(
    const DexType* type) const {
  if (!m_create_init_class_insns) {
    return nullptr;
  }
  type = refine(type);
  if (!type) {
    return nullptr;
  }
  return (new IRInstruction(IOPCODE_INIT_CLASS))
      ->set_type(const_cast<DexType*>(type));
}

} // namespace init_classes
