/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <vector>

#include "ConcurrentContainers.h"
#include "DexClass.h"
#include "MethodUtil.h"
#include "Walkers.h"

namespace init_classes {

using InitClasses = std::vector<const DexClass*>;

/**
 * For a given scope, this class provides information about which static
 * initializer with side effects get triggered when some class is initialized.
 */
class InitClassesWithSideEffects {
 private:
  ConcurrentMap<const DexType*, std::shared_ptr<InitClasses>> m_init_classes;
  InitClasses m_empty_init_classes;
  bool m_create_init_class_insns;

  const InitClasses* compute(const DexClass* cls) {
    auto res =
        m_init_classes.get(cls->get_type(), std::shared_ptr<InitClasses>());
    if (res) {
      return res.get();
    }
    InitClasses classes;
    const auto* refined_cls = method::clinit_may_have_side_effects(cls);
    if (refined_cls == nullptr) {
    } else if (refined_cls != cls) {
      classes = *compute(refined_cls);
    } else {
      classes.push_back(cls);
      auto super_cls = type_class(cls->get_super_class());
      if (super_cls) {
        const auto super_classes = compute(super_cls);
        classes.insert(classes.end(), super_classes->begin(),
                       super_classes->end());
      }
    }
    m_init_classes.update(
        cls->get_type(),
        [&res, &classes](const DexType*, std::shared_ptr<InitClasses>& value,
                         bool exist) {
          if (exist) {
            always_assert(classes == *value);
          } else {
            value = std::make_shared<InitClasses>(std::move(classes));
          }
          res = value;
        });
    return res.get();
  }

 public:
  InitClassesWithSideEffects(const Scope& scope, bool create_init_class_insns)
      : m_create_init_class_insns(create_init_class_insns) {
    walk::parallel::classes(scope, [&](const DexClass* cls) { compute(cls); });
  }

  // Determine list of classes with static initializers with side effects that
  // would get triggered when the given type is initialized. The list is ordered
  // such that base types come later.
  const InitClasses* get(const DexType* type) const {
    auto it = m_init_classes.find(type);
    return it == m_init_classes.end() ? &m_empty_init_classes
                                      : it->second.get();
  }

  // Given a type to be initialized, determine the most derived class with a
  // static initializer with side effects that would get triggered, if any.
  const DexType* refine(const DexType* type) const {
    auto init_classes = get(type);
    return init_classes->empty() ? nullptr : init_classes->front()->get_type();
  }

  // Given a type to be initialized, create an init-class instruction with the
  // most derived class static initializer with side effects that would get
  // triggered. If there is no such class, returns nullptr.
  IRInstruction* create_init_class_insn(const DexType* type) const {
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
};

} // namespace init_classes
