/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "CallSiteSummaries.h"

namespace inliner {

class RecursionPruner {
 private:
  MethodToMethodOccurrences& m_callee_caller;
  MethodToMethodOccurrences& m_caller_callee;
  size_t m_recursive_call_sites{0};
  size_t m_max_call_stack_depth{0};
  std::unordered_set<const DexMethod*> m_recursive_callees;
  std::unordered_set<const DexMethod*> m_excluded_callees;
  std::function<bool(DexMethod*, DexMethod*)> m_exclude_fn;

 public:
  RecursionPruner(MethodToMethodOccurrences& callee_caller,
                  MethodToMethodOccurrences& caller_callee,
                  std::function<bool(DexMethod*, DexMethod*)> exclude_fn);

  void run();

  size_t recursive_call_sites() const { return m_recursive_call_sites; }
  size_t max_call_stack_depth() const { return m_max_call_stack_depth; }

  std::unordered_set<const DexMethod*>& recursive_callees() {
    return m_recursive_callees;
  }

  std::unordered_set<const DexMethod*>& excluded_callees() {
    return m_excluded_callees;
  }

 private:
  size_t recurse(DexMethod* caller,
                 std::unordered_map<DexMethod*, size_t>* visited);
};

} // namespace inliner
