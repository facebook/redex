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
#include "IRInstruction.h"

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

  const InitClasses* compute(const DexClass* cls);

 public:
  InitClassesWithSideEffects(const Scope& scope, bool create_init_class_insns);

  // Determine list of classes with static initializers with side effects that
  // would get triggered when the given type is initialized. The list is ordered
  // such that base types come later.
  const InitClasses* get(const DexType* type) const;

  // Given a type to be initialized, determine the most derived class with a
  // static initializer with side effects that would get triggered, if any.
  const DexType* refine(const DexType* type) const;

  // Given a type to be initialized, create an init-class instruction with the
  // most derived class static initializer with side effects that would get
  // triggered. If there is no such class, returns nullptr.
  IRInstruction* create_init_class_insn(const DexType* type) const;
};

} // namespace init_classes
