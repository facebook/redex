/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "InitClassesWithSideEffects.h"

namespace init_classes {

struct Stats {
  size_t init_class_instructions{0};
  size_t init_class_instructions_removed{0};
  size_t init_class_instructions_refined{0};

  Stats& operator+=(const Stats&);
};

class InitClassPruner {
 public:
  InitClassPruner(
      const InitClassesWithSideEffects& init_classes_with_side_effects,
      const DexType* declaring_type,
      cfg::ControlFlowGraph& cfg);

  const Stats& get_stats() const { return m_stats; }

  void apply();

 private:
  void apply_forward();
  void apply_backward();
  const InitClassesWithSideEffects& m_init_classes_with_side_effects;
  const DexType* m_declaring_type;
  cfg::ControlFlowGraph& m_cfg;
  Stats m_stats;
};

} // namespace init_classes
