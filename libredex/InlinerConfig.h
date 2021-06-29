/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "ShrinkerConfig.h"

namespace inliner {

/**
 * The global inliner config.
 */
struct InlinerConfig {
  // inline virtual methods
  bool virtual_inline{true};
  bool true_virtual_inline{false};
  bool throws_inline{false};
  bool throw_after_no_return{false};
  bool enforce_method_size_limit{true};
  bool multiple_callers{false};
  bool delete_any_candidate{false};
  bool use_constant_propagation_and_local_dce_for_callee_size{true};
  bool use_cfg_inliner{false};
  bool intermediate_shrinking{false};
  shrinker::ShrinkerConfig shrinker;
  bool shrink_other_methods{true};
  bool unique_inlined_registers{true};
  bool debug{false};

  /*
   * Some versions of ART (5.0.0 - 5.0.2) will fail to verify a method if it
   * is too large. See https://code.google.com/p/android/issues/detail?id=66655.
   *
   * The verifier rounds up to the next power of two, and doesn't support any
   * size greater than 16. See
   * http://androidxref.com/5.0.0_r2/xref/art/compiler/dex/verified_method.cc#107
   */
  uint64_t soft_max_instruction_size{1 << 15};
  // INSTRUCTION_BUFFER is added because the final method size is often larger
  // than our estimate -- during the sync phase, we may have to pick larger
  // branch opcodes to encode large jumps.
  uint64_t instruction_size_buffer{1 << 12};

  std::unordered_set<DexType*> allowlist_no_method_limit;
  // We will populate the information to rstate of classes and methods.
  std::unordered_set<DexType*> m_no_inline_annos;
  std::unordered_set<DexType*> m_force_inline_annos;
  // Prefixes of classes not to inline from / into
  std::vector<std::string> m_blocklist;
  std::vector<std::string> m_caller_blocklist;
  std::vector<std::string> m_intradex_allowlist;

  /**
   * 1. Populate m_blocklist m_caller_blocklist to blocklist and
   * caller_blocklist with the initial scope.
   * 2. Set rstate of classes and methods if they are annotated by any
   * m_no_inline_annos and m_force_inline_annos.
   */
  void populate(const Scope& scope);

  const std::unordered_set<DexType*>& get_blocklist() const {
    always_assert_log(m_populated, "Should populate blocklist\n");
    return blocklist;
  }

  const std::unordered_set<DexType*>& get_caller_blocklist() const {
    always_assert_log(m_populated, "Should populate blocklist\n");
    return caller_blocklist;
  }

  void apply_intradex_allowlist() {
    always_assert_log(m_populated, "Should populate allowlist\n");
    for (auto type : intradex_allowlist) {
      blocklist.erase(type);
      caller_blocklist.erase(type);
    }
  }

 private:
  bool m_populated{false};
  // The populated black lists.
  std::unordered_set<DexType*> blocklist;
  std::unordered_set<DexType*> caller_blocklist;
  std::unordered_set<DexType*> intradex_allowlist;
};
} // namespace inliner
