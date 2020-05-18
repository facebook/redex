/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "LocalTypeAnalyzer.h"
#include "WholeProgramState.h"

namespace type_analyzer {

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

  void apply(const local::LocalTypeAnalyzer&,
             const WholeProgramState&,
             DexMethod*);

 private:
  ir_list::InstructionIterator insert_field_assert(
      const WholeProgramState&, IRCode*, ir_list::InstructionIterator);
  ir_list::InstructionIterator insert_return_value_assert(
      const WholeProgramState&, IRCode*, ir_list::InstructionIterator);

  Config m_config;
};

} // namespace type_analyzer
