/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationWholeProgramState.h"
#include "DexClass.h"

namespace constant_propagation {

/*
 * This class inserts runtime assertions that check that the arguments, fields,
 * and return values that our static analysis thinks are constant actually have
 * those values at runtime.
 */
class RuntimeAssertTransform {
 public:
  struct Config {
    DexMethodRef* param_assert_fail_handler{nullptr};
    DexMethodRef* field_assert_fail_handler{nullptr};
    DexMethodRef* return_value_assert_fail_handler{nullptr};
    Config() = default;
    Config(const ProguardMap&);
  };

  RuntimeAssertTransform(const Config& config) : m_config(config) {}

  void apply(const intraprocedural::FixpointIterator&,
             const WholeProgramState&,
             DexMethod*);

 private:
  ir_list::InstructionIterator insert_field_assert(
      const WholeProgramState&, IRCode*, ir_list::InstructionIterator);
  ir_list::InstructionIterator insert_return_value_assert(
      const WholeProgramState&, IRCode*, ir_list::InstructionIterator);

  void insert_param_asserts(const ConstantEnvironment&, DexMethod* method);

  Config m_config;
};

} // namespace constant_propagation
