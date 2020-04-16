/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace inliner {

/**
 * The global inliner config.
 */
struct InlinerConfig {
  // inline virtual methods
  bool virtual_inline{true};
  bool true_virtual_inline{false};
  bool throws_inline{false};
  bool enforce_method_size_limit{true};
  bool multiple_callers{false};
  bool inline_small_non_deletables{true};
  bool use_constant_propagation_for_callee_size{true};
  bool use_cfg_inliner{false};
  bool run_const_prop{false};
  bool run_cse{false};
  bool run_copy_prop{false};
  bool run_local_dce{false};
  bool run_dedup_blocks{false};
  bool shrink_other_methods{true};
  bool debug{false};
  std::unordered_set<DexType*> whitelist_no_method_limit;
  // We will populate the information to rstate of classes and methods.
  std::unordered_set<DexType*> m_no_inline_annos;
  std::unordered_set<DexType*> m_force_inline_annos;
  // Prefixes of classes not to inline from / into
  std::vector<std::string> m_black_list;
  std::vector<std::string> m_caller_black_list;

  /**
   * 1. Populate m_black_list m_caller_black_list to black_list and
   * caller_black_list with the initial scope.
   * 2. Set rstate of classes and methods if they are annotated by any
   * m_no_inline_annos and m_force_inline_annos.
   */
  void populate(const Scope& scope);

  const std::unordered_set<DexType*>& get_black_list() const {
    always_assert_log(m_populated, "Should populate blacklist\n");
    return black_list;
  }

  const std::unordered_set<DexType*>& get_caller_black_list() const {
    always_assert_log(m_populated, "Should populate blacklist\n");
    return caller_black_list;
  }

 private:
  bool m_populated{false};
  // The populated black lists.
  std::unordered_set<DexType*> black_list;
  std::unordered_set<DexType*> caller_black_list;
};
} // namespace inliner
