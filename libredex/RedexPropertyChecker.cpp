/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RedexPropertyChecker.h"
#include "RedexPropertyCheckerRegistry.h"

namespace redex_properties {

PropertyChecker::PropertyChecker(Property property) : m_property(property) {
  PropertyCheckerRegistry::get().register_checker(this);
}

PropertyChecker::~PropertyChecker() {}

} // namespace redex_properties
