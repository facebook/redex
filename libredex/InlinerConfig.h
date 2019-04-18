/**
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
  bool throws_inline{false};
  bool enforce_method_size_limit{true};
  bool multiple_callers{false};
  bool inline_small_non_deletables{false};
  bool use_cfg_inliner{false};
  std::unordered_set<DexType*> whitelist_no_method_limit;
  std::unordered_set<DexType*> no_inline;
  std::unordered_set<DexType*> force_inline;
  // Prefixes of classes not to inline from / into
  std::vector<std::string> m_black_list;
  std::vector<std::string> m_caller_black_list;

  /**
   * Populate m_black_list m_caller_black_list to black_list and
   * caller_black_list with the initial scope.
   */
  void populate_blacklist(const Scope& scope);

  const std::unordered_set<DexType*>& get_black_list() const {
    always_assert_log(populated, "Should populate blacklist\n");
    return black_list;
  }

  const std::unordered_set<DexType*>& get_caller_black_list() const {
    always_assert_log(populated, "Should populate blacklist\n");
    return caller_black_list;
  }

 private:
  bool populated{false};
  // The populated black lists.
  std::unordered_set<DexType*> black_list;
  std::unordered_set<DexType*> caller_black_list;
};
} // namespace inliner
