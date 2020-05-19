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

  struct Stats {
    size_t field_type_check_inserted{0};
    size_t return_type_check_inserted{0};

    Stats& operator+=(const Stats& that) {
      field_type_check_inserted += that.field_type_check_inserted;
      return_type_check_inserted += that.return_type_check_inserted;
      return *this;
    }

    void report(PassManager& mgr) const {
      mgr.incr_metric("field_type_check_inserted", field_type_check_inserted);
      mgr.incr_metric("return_type_check_inserted", return_type_check_inserted);
      TRACE(TYPE, 2, "[type-analysis] RuntimeAssert Stats:");
      TRACE(TYPE,
            2,
            "[type-analysis] field_type_check_inserted = %u",
            field_type_check_inserted);
      TRACE(TYPE,
            2,
            "[type-analysis] return_type_check_inserted = %u",
            return_type_check_inserted);
    }
  };

  explicit RuntimeAssertTransform(const Config& config) : m_config(config) {}

  RuntimeAssertTransform::Stats apply(const local::LocalTypeAnalyzer&,
                                      const WholeProgramState&,
                                      DexMethod*);

 private:
  ir_list::InstructionIterator insert_field_assert(const WholeProgramState&,
                                                   IRCode*,
                                                   ir_list::InstructionIterator,
                                                   Stats&);
  ir_list::InstructionIterator insert_return_value_assert(
      const WholeProgramState&, IRCode*, ir_list::InstructionIterator, Stats&);

  Config m_config;
};

} // namespace type_analyzer
