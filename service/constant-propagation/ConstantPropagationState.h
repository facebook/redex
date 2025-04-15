/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DeterministicContainers.h"
#include "DexClass.h"

namespace constant_propagation {

class State {
 public:
  const UnorderedSet<DexMethodRef*>& kotlin_null_check_assertions() const {
    return m_kotlin_null_check_assertions;
  }

  const DexMethodRef* redex_null_check_assertion() const {
    return m_redex_null_check_assertion;
  }

  State();

 private:
  UnorderedSet<DexMethodRef*> m_kotlin_null_check_assertions;
  const DexMethodRef* m_redex_null_check_assertion;
};

} // namespace constant_propagation
