/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationState.h"
#include "KotlinNullCheckMethods.h"
#include "MethodUtil.h"

namespace constant_propagation {

State::State()
    : m_kotlin_null_check_assertions(
          kotlin_nullcheck_wrapper::get_kotlin_null_assertions()),
      m_redex_null_check_assertion(
          method::redex_internal_checkObjectNotNull()) {}

} // namespace constant_propagation
