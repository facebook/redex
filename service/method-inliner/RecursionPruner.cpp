/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RecursionPruner.h"

namespace inliner {

RecursionPruner::RecursionPruner(
    MethodToMethodOccurrences& callee_caller,
    MethodToMethodOccurrences& caller_callee,
    std::function<bool(DexMethod*, DexMethod*)> exclude_fn)
    : m_callee_caller(callee_caller),
      m_caller_callee(caller_callee),
      m_exclude_fn(std::move(exclude_fn)) {}

void RecursionPruner::run() {
  Timer t("compute_caller_nonrecursive_callees_by_stack_depth");
  // we want to inline bottom up, so as a first step, for all callers, we
  // recurse into all inlinable callees until we hit a leaf and we start
  // inlining from there. First, we just gather data on
  // caller/non-recursive-callees pairs for each stack depth.
  std::unordered_map<DexMethod*, size_t> visited;
  std::vector<DexMethod*> ordered_callers;
  ordered_callers.reserve(m_caller_callee.size());
  for (auto& p : m_caller_callee) {
    ordered_callers.push_back(const_cast<DexMethod*>(p.first));
  }
  std::sort(ordered_callers.begin(), ordered_callers.end(), compare_dexmethods);
  for (const auto caller : ordered_callers) {
    TraceContext context(caller);
    auto stack_depth = recurse(caller, &visited);
    m_max_call_stack_depth = std::max(m_max_call_stack_depth, stack_depth);
  }
}

size_t RecursionPruner::recurse(
    DexMethod* caller, std::unordered_map<DexMethod*, size_t>* visited) {
  auto caller_it = m_caller_callee.find(caller);
  if (caller_it == m_caller_callee.end()) {
    return 0;
  }
  auto& callees = caller_it->second;
  always_assert(!callees.empty());

  auto visited_it = visited->find(caller);
  if (visited_it != visited->end()) {
    return visited_it->second;
  }

  // We'll only know the exact call stack depth at the end.
  visited->emplace(caller, std::numeric_limits<size_t>::max());

  std::vector<DexMethod*> ordered_callees;
  ordered_callees.reserve(callees.size());
  for (auto& p : callees) {
    ordered_callees.push_back(p.first);
  }
  std::sort(ordered_callees.begin(), ordered_callees.end(), compare_dexmethods);
  size_t stack_depth = 0;
  // recurse into the callees in case they have something to inline on
  // their own. We want to inline bottom up so that a callee is
  // completely resolved by the time it is inlined.
  for (auto callee : ordered_callees) {
    size_t callee_stack_depth = recurse(callee, visited);
    if (callee_stack_depth == std::numeric_limits<size_t>::max()) {
      // we've found recursion in the current call stack
      m_recursive_call_sites += callees.at(callee);
      m_recursive_callees.insert(callee);
    } else {
      stack_depth = std::max(stack_depth, callee_stack_depth + 1);
      if (!m_exclude_fn(caller, callee)) {
        continue;
      }
      m_excluded_callees.insert(callee);
    }

    // If we get here, we shall prune the (caller, callee) pair.
    callees.erase(callee);
    if (callees.empty()) {
      m_caller_callee.erase(caller);
    }
    auto& callers = m_callee_caller.at(callee);
    callers.erase(caller);
    if (callers.empty()) {
      m_callee_caller.erase(callee);
    }
  }

  (*visited)[caller] = stack_depth;
  return stack_depth;
}

} // namespace inliner
