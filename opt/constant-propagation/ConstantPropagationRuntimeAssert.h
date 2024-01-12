/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationWholeProgramState.h"
#include "DexClass.h"

struct ProguardMap;

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
    explicit Config(const ProguardMap&);
  };

  explicit RuntimeAssertTransform(const Config& config) : m_config(config) {}

  void apply(const intraprocedural::FixpointIterator&,
             const WholeProgramState&,
             DexMethod*);

 private:
  // \returns true if any insn is inserted.
  bool insert_field_assert(const WholeProgramState&,
                           cfg::ControlFlowGraph&,
                           cfg::InstructionIterator&);
  // \returns true if any insns is instered.
  bool insert_return_value_assert(const WholeProgramState&,
                                  cfg::ControlFlowGraph&,
                                  cfg::InstructionIterator&);

  void insert_param_asserts(const ConstantEnvironment&, DexMethod* method);

  Config m_config;
};

} // namespace constant_propagation
