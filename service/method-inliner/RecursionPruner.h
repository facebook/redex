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
  ConcurrentMethodToMethodOccurrences& m_callee_caller;
  ConcurrentMethodToMethodOccurrences& m_caller_callee;
  size_t m_recursive_call_sites{0};
  size_t m_max_call_stack_depth{0};
  UnorderedSet<const DexMethod*> m_recursive_callees;
  UnorderedSet<const DexMethod*> m_excluded_callees;
  std::function<bool(DexMethod*, DexMethod*)> m_exclude_fn;

 public:
  RecursionPruner(ConcurrentMethodToMethodOccurrences& callee_caller,
                  ConcurrentMethodToMethodOccurrences& caller_callee,
                  std::function<bool(DexMethod*, DexMethod*)> exclude_fn);

  void run();

  size_t recursive_call_sites() const { return m_recursive_call_sites; }
  size_t max_call_stack_depth() const { return m_max_call_stack_depth; }

  UnorderedSet<const DexMethod*>& recursive_callees() {
    return m_recursive_callees;
  }

  UnorderedSet<const DexMethod*>& excluded_callees() {
    return m_excluded_callees;
  }

 private:
  size_t recurse(DexMethod* caller, UnorderedMap<DexMethod*, size_t>* visited);
};

} // namespace inliner
