/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RedexPropertyChecker.h"
#include "RedexPropertyCheckerRegistry.h"

namespace redex_properties {

PropertyChecker::PropertyChecker(std::string property_name)
    : m_property_name(std::move(property_name)) {
  PropertyCheckerRegistry::get().register_checker(this);
}

PropertyChecker::~PropertyChecker() {}

} // namespace redex_properties
