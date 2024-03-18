/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Shrinker.h"

using CallSiteArguments = constant_propagation::interprocedural::ArgumentDomain;

struct CallSiteSummary {
  CallSiteArguments arguments;
  bool result_used;

  /*
   * The key of a call-site-summary is a canonical string representation of the
   * constant arguments. Usually, the string is quite small, it only rarely
   * contains fields or methods.
   */
  std::string get_key() const;

  static void append_key_value(std::ostringstream& oss,
                               const ConstantValue& value);
};

inline bool operator==(const CallSiteSummary& a, const CallSiteSummary& b) {
  return a.result_used == b.result_used && a.arguments.equals(b.arguments);
}

struct CalleeCallSiteSummary {
  const DexMethod* method;
  const CallSiteSummary* call_site_summary;
};

inline size_t hash_value(CalleeCallSiteSummary ccss) {
  return ((size_t)ccss.method) ^ (size_t)(ccss.call_site_summary);
}

inline bool operator==(const CalleeCallSiteSummary& a,
                       const CalleeCallSiteSummary& b) {
  return a.method == b.method && a.call_site_summary == b.call_site_summary;
}

using InvokeCallSiteSummaries =
    std::vector<std::pair<IRInstruction*, const CallSiteSummary*>>;

struct InvokeCallSiteSummariesAndDeadBlocks {
  InvokeCallSiteSummaries invoke_call_site_summaries;
  size_t dead_blocks{0};
};

using CallSiteSummaryOccurrences = std::pair<const CallSiteSummary*, size_t>;

using ConcurrentMethodToMethodOccurrences =
    ConcurrentMap<const DexMethod*, std::unordered_map<DexMethod*, size_t>>;

namespace inliner {

using GetCalleeFunction = std::function<DexMethod*(DexMethod*, IRInstruction*)>;
using HasCalleeOtherCallSitesPredicate = std::function<bool(DexMethod*)>;

struct CallSiteSummaryStats {
  std::atomic<size_t> constant_invoke_callers_unreachable{0};
  std::atomic<size_t> constant_invoke_callers_analyzed{0};
  std::atomic<size_t> constant_invoke_callers_unreachable_blocks{0};
  std::atomic<size_t> constant_invoke_callers_critical_path_length{0};
};

class CallSiteSummarizer {
  shrinker::Shrinker& m_shrinker;
  const ConcurrentMethodToMethodOccurrences& m_callee_caller;
  const ConcurrentMethodToMethodOccurrences& m_caller_callee;
  GetCalleeFunction m_get_callee_fn;
  HasCalleeOtherCallSitesPredicate m_has_callee_other_call_sites_fn;
  std::function<bool(const ConstantValue&)>* m_filter_fn;
  CallSiteSummaryStats* m_stats;

  struct CalleeInfo {
    std::unordered_map<const CallSiteSummary*, size_t> indices;
    std::vector<CallSiteSummaryOccurrences> occurrences;
    std::vector<const IRInstruction*> invokes;
  };

  /**
   * For all (reachable) invoked methods, call-site summaries and invoke
   * instructions
   */
  ConcurrentMap<const DexMethod*, CalleeInfo> m_callee_infos;

  /**
   * For all (reachable) invoke instructions, constant arguments
   */
  InsertOnlyConcurrentMap<const IRInstruction*, const CallSiteSummary*>
      m_invoke_call_site_summaries;

  /**
   * Internalized call-site summaries.
   */
  InsertOnlyConcurrentMap<std::string, CallSiteSummary> m_call_site_summaries;

  /**
   * For all (reachable) invoke instructions in a given method, collect
   * information about their arguments, i.e. whether particular arguments
   * are constants.
   */
  InvokeCallSiteSummariesAndDeadBlocks get_invoke_call_site_summaries(
      DexMethod* caller,
      const std::unordered_map<DexMethod*, size_t>& callees,
      const ConstantEnvironment& initial_env);

 public:
  CallSiteSummarizer(
      shrinker::Shrinker& shrinker,
      const ConcurrentMethodToMethodOccurrences& callee_caller,
      const ConcurrentMethodToMethodOccurrences& caller_callee,
      GetCalleeFunction get_callee_fn,
      HasCalleeOtherCallSitesPredicate has_callee_other_call_sites_fn,
      std::function<bool(const ConstantValue&)>* filter_fn,
      CallSiteSummaryStats* stats);

  void summarize();

  const CallSiteSummary* internalize_call_site_summary(
      const CallSiteSummary& call_site_summary);

  const std::vector<CallSiteSummaryOccurrences>*
  get_callee_call_site_summary_occurrences(const DexMethod* callee) const;

  const std::vector<const IRInstruction*>* get_callee_call_site_invokes(
      const DexMethod* callee) const;

  const CallSiteSummary* get_instruction_call_site_summary(
      const IRInstruction* invoke_insn) const;
};

} // namespace inliner
