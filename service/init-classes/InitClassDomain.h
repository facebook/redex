/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sparta/AbstractDomain.h>
#include <sparta/PatriciaTreeSetAbstractDomain.h>

#include "DexClass.h"
#include "InitClassesWithSideEffects.h"

namespace init_classes {

using PatriciaClasses = sparta::PatriciaTreeSet<const DexClass*>;

// A set of classes whose static initializer would have been triggered at some
// point of the program execution.
class InitClassDomain final
    : public sparta::AbstractDomainReverseAdaptor<
          sparta::PatriciaTreeSetAbstractDomain<const DexClass*>,
          InitClassDomain> {
 public:
  using AbstractDomainReverseAdaptor::AbstractDomainReverseAdaptor;

  // Some older compilers complain that the class is not default constructible.
  // We intended to use the default constructors of the base class (via using
  // AbstractDomainReverseAdaptor::AbstractDomainReverseAdaptor), but some
  // compilers fail to catch this. So we insert a redundant '= default'.
  InitClassDomain() = default;

  const PatriciaClasses& elements() const { return unwrap().elements(); }

  // Finds and inserts all classes initialized from the given class
  void insert(const InitClassesWithSideEffects& init_classes_with_side_effects,
              const DexType* type) {
    if (is_bottom()) {
      return;
    }
    for (auto init_cls : *init_classes_with_side_effects.get(type)) {
      if (unwrap().contains(init_cls)) {
        break;
      }
      always_assert(!init_cls->is_external());
      if (unwrap().is_bottom()) {
        *this = InitClassDomain(init_cls);
      } else {
        unwrap().add(init_cls);
      }
    }
  }
};

} // namespace init_classes
