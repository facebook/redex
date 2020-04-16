/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "IRCode.h"

namespace copy_propagation_impl {

struct Config {
  bool eliminate_const_literals{false};
  bool eliminate_const_literals_with_same_type_demands{true};
  bool eliminate_const_strings{true};
  bool eliminate_const_classes{true};
  bool replace_with_representative{true};
  bool wide_registers{true};
  bool static_finals{true};
  bool debug{false};

  // this is set by PassManager, not by JsonWrapper
  bool regalloc_has_run{false};
  unsigned int max_estimated_registers{3000};
};

struct Stats {
  size_t moves_eliminated{0};
  size_t replaced_sources{0};
  size_t skipped_due_to_too_many_registers{0};
  size_t type_inferences{0};

  Stats() = default;
  Stats(size_t elim, size_t replaced, size_t skipped)
      : moves_eliminated(elim),
        replaced_sources(replaced),
        skipped_due_to_too_many_registers(skipped) {}

  Stats& operator+=(const Stats&);
};

class CopyPropagation final {
 public:
  explicit CopyPropagation(const Config& config) : m_config(config) {}

  Stats run(const Scope& scope);

  Stats run(IRCode*, DexMethod* = nullptr);

 private:
  const Config& m_config;
};

} // namespace copy_propagation_impl
