/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RedexPropertyCheckerRegistry.h"

namespace redex_properties {

PropertyCheckerRegistry& PropertyCheckerRegistry::get() {
  static PropertyCheckerRegistry registry;
  return registry;
}

void PropertyCheckerRegistry::register_checker(PropertyChecker* checker) {
  m_registered_checkers.push_back(checker);
}

const std::vector<PropertyChecker*>& PropertyCheckerRegistry::get_checkers()
    const {
  return m_registered_checkers;
}

} // namespace redex_properties
