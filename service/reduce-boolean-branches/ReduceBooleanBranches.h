/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ABExperimentContext.h"
#include "DexClass.h"

namespace reduce_boolean_branches_impl {

struct Config {};

struct Stats {
  size_t boolean_branches_removed{0};
  size_t object_branches_removed{0};
  size_t xors_reduced{0};
  Stats& operator+=(const Stats& that);
};

class ReduceBooleanBranches {
 public:
  ReduceBooleanBranches(const Config& config,
                        bool is_static,
                        DexTypeList* args,
                        IRCode* code,
                        const std::function<void()>* on_change = nullptr);

  const Stats& get_stats() const { return m_stats; }

  bool run();

 private:
  bool reduce_diamonds();
  bool reduce_xors();
  void ensure_experiment();

  const Config& m_config;
  bool m_is_static;
  DexTypeList* m_args;
  IRCode* m_code;
  Stats m_stats;
  const std::function<void()>* m_on_change;
};

} // namespace reduce_boolean_branches_impl
