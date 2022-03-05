/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CallSiteSummaries.h"

#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationWholeProgramState.h"
#include "PriorityThreadPoolDAGScheduler.h"
#include "Timer.h"
#include "WorkQueue.h"

std::string CallSiteSummary::get_key() const {
  always_assert(!arguments.is_bottom());
  static std::array<std::string, 2> result_used_strs = {"-", "+"};
  if (arguments.is_top()) {
    return result_used_strs[result_used];
  }
  const auto& bindings = arguments.bindings();
  std::vector<reg_t> ordered_arg_idxes;
  for (auto& p : bindings) {
    ordered_arg_idxes.push_back(p.first);
  }
  always_assert(!ordered_arg_idxes.empty());
  std::sort(ordered_arg_idxes.begin(), ordered_arg_idxes.end());
  std::ostringstream oss;
  oss << result_used_strs[result_used];
  for (auto arg_idx : ordered_arg_idxes) {
    if (arg_idx != ordered_arg_idxes.front()) {
      oss << ",";
    }
    oss << arg_idx << ":";
    const auto& value = bindings.at(arg_idx);
    if (const auto& signed_value = value.maybe_get<SignedConstantDomain>()) {
      auto c = signed_value->get_constant();
      if (c) {
        oss << *c;
      } else {
        oss << show(*signed_value);
      }
    } else if (const auto& singleton_value =
                   value.maybe_get<SingletonObjectDomain>()) {
      auto field = *singleton_value->get_constant();
      oss << show(field);
    } else if (const auto& obj_or_none =
                   value.maybe_get<ObjectWithImmutAttrDomain>()) {
      auto object = obj_or_none->get_constant();
      if (object->jvm_cached_singleton) {
        oss << "(cached)";
      }
      oss << show(object->type);
      oss << "{";
      bool first{true};
      for (auto& attr : object->attributes) {
        if (first) {
          first = false;
        } else {
          oss << ",";
        }
        if (attr.attr.is_field()) {
          oss << show(attr.attr.field);
        } else {
          always_assert(attr.attr.is_method());
          oss << show(attr.attr.method);
        }
        oss << "=";
        if (const auto& signed_value2 =
                attr.value.maybe_get<SignedConstantDomain>()) {
          auto c = signed_value2->get_constant();
          if (c) {
            oss << *c;
          } else {
            oss << show(*signed_value2);
          }
        }
      }
      oss << "}";
    } else {
      not_reached_log("unexpected value: %s", SHOW(value));
    }
  }
  return oss.str();
}

namespace inliner {

CallSiteSummarizer::CallSiteSummarizer(
    shrinker::Shrinker& shrinker,
    const MethodToMethodOccurrences& callee_caller,
    const MethodToMethodOccurrences& caller_callee,
    GetCalleeFunction get_callee_fn,
    HasCalleeOtherCallSitesPredicate has_callee_other_call_sites_fn,
    CallSiteSummaryStats* stats)
    : m_shrinker(shrinker),
      m_callee_caller(callee_caller),
      m_caller_callee(caller_callee),
      m_get_callee_fn(std::move(get_callee_fn)),
      m_has_callee_other_call_sites_fn(
          std::move(has_callee_other_call_sites_fn)),
      m_stats(stats) {}

const CallSiteSummary* CallSiteSummarizer::internalize_call_site_summary(
    const CallSiteSummary& call_site_summary) {
  auto key = call_site_summary.get_key();
  const CallSiteSummary* res;
  m_call_site_summaries.update(
      key, [&](const std::string&,
               std::unique_ptr<const CallSiteSummary>& p,
               bool exist) {
        if (exist) {
          always_assert_log(p->result_used == call_site_summary.result_used,
                            "same key %s for\n    %d\nvs. %d", key.c_str(),
                            p->result_used, call_site_summary.result_used);
          always_assert_log(p->arguments.equals(call_site_summary.arguments),
                            "same key %s for\n    %s\nvs. %s", key.c_str(),
                            SHOW(p->arguments),
                            SHOW(call_site_summary.arguments));
        } else {
          p = std::make_unique<const CallSiteSummary>(call_site_summary);
        }
        res = p.get();
      });
  return res;
}

void CallSiteSummarizer::summarize() {
  Timer t("compute_call_site_summaries");
  struct CalleeInfo {
    std::unordered_map<const CallSiteSummary*, size_t> occurrences;
    std::vector<IRInstruction*> invokes;
  };

  // We'll do a top-down traversal of all call-sites, in order to propagate
  // call-site information from outer call-sites to nested call-sites, improving
  // the precision of the analysis. This is effectively an inter-procedural
  // constant-propagation analysis, but we are operating on a reduced call-graph
  // as recursion has been broken by eliminating some call-sites from
  // consideration, and we are only considering those methods that are involved
  // in an inlinable caller-callee relationship, which excludes much of true
  // virtual methods.
  PriorityThreadPoolDAGScheduler<DexMethod*> summaries_scheduler;

  ConcurrentMap<DexMethod*, std::shared_ptr<CalleeInfo>>
      concurrent_callee_infos;

  // Helper function to retrieve a list of callers of a callee such that all
  // possible call-sites to the callee are in the returned callers.
  auto get_dependencies =
      [&](DexMethod* callee) -> const std::unordered_map<DexMethod*, size_t>* {
    auto it = m_callee_caller.find(callee);
    if (it == m_callee_caller.end() ||
        m_has_callee_other_call_sites_fn(callee)) {
      return nullptr;
    }
    // If we get here, then we know all possible call-sites to the callee, and
    // they reside in the known list of callers.
    return &it->second;
  };

  summaries_scheduler.set_executor([&](DexMethod* method) {
    CallSiteArguments arguments;
    if (!get_dependencies(method)) {
      // There are no relevant callers from which we could gather incoming
      // constant arguments.
      arguments = CallSiteArguments::top();
    } else {
      auto ci = concurrent_callee_infos.get(method, nullptr);
      if (!ci) {
        // All callers were unreachable
        arguments = CallSiteArguments::bottom();
      } else {
        // The only way to call this method is by going through a set of known
        // call-sites. We join together all those incoming constant arguments.
        always_assert(!ci->occurrences.empty());
        auto it = ci->occurrences.begin();
        arguments = it->first->arguments;
        for (it++; it != ci->occurrences.end(); it++) {
          arguments.join_with(it->first->arguments);
        }
      }
    }

    if (arguments.is_bottom()) {
      // unreachable
      m_stats->constant_invoke_callers_unreachable++;
      return;
    }
    auto& callees = m_caller_callee.at(method);
    ConstantEnvironment initial_env =
        constant_propagation::interprocedural::env_with_params(
            is_static(method), method->get_code(), arguments);
    auto res = get_invoke_call_site_summaries(method, callees, initial_env);
    for (auto& p : res.invoke_call_site_summaries) {
      auto insn = p.first;
      auto callee = m_get_callee_fn(method, insn);
      auto call_site_summary = p.second;
      concurrent_callee_infos.update(callee,
                                     [&](const DexMethod*,
                                         std::shared_ptr<CalleeInfo>& ci,
                                         bool /* exists */) {
                                       if (!ci) {
                                         ci = std::make_shared<CalleeInfo>();
                                       }
                                       ci->occurrences[call_site_summary]++;
                                       ci->invokes.push_back(insn);
                                     });
      m_invoke_call_site_summaries.emplace(insn, call_site_summary);
    }
    m_stats->constant_invoke_callers_analyzed++;
    m_stats->constant_invoke_callers_unreachable_blocks += res.dead_blocks;
  });

  std::vector<DexMethod*> callers;
  for (auto& p : m_caller_callee) {
    auto method = const_cast<DexMethod*>(p.first);
    callers.push_back(method);
    auto dependencies = get_dependencies(method);
    if (dependencies) {
      for (auto& q : *dependencies) {
        summaries_scheduler.add_dependency(method, q.first);
      }
    }
  }
  m_stats->constant_invoke_callers_critical_path_length =
      summaries_scheduler.run(callers.begin(), callers.end());

  for (auto& p : concurrent_callee_infos) {
    auto callee = p.first;
    auto& v = m_callee_call_site_summary_occurrences[callee];
    auto& ci = p.second;
    for (const auto& q : ci->occurrences) {
      const auto call_site_summary = q.first;
      const auto count = q.second;
      v.emplace_back(call_site_summary, count);
    }
    auto& invokes = m_callee_call_site_invokes[callee];
    invokes.insert(invokes.end(), ci->invokes.begin(), ci->invokes.end());
  }
}

InvokeCallSiteSummariesAndDeadBlocks
CallSiteSummarizer::get_invoke_call_site_summaries(
    DexMethod* caller,
    const std::unordered_map<DexMethod*, size_t>& callees,
    const ConstantEnvironment& initial_env) {
  IRCode* code = caller->get_code();

  InvokeCallSiteSummariesAndDeadBlocks res;
  auto& cfg = code->cfg();
  constant_propagation::intraprocedural::FixpointIterator intra_cp(
      cfg,
      constant_propagation::ConstantPrimitiveAndBoxedAnalyzer(
          m_shrinker.get_immut_analyzer_state(),
          m_shrinker.get_immut_analyzer_state(),
          constant_propagation::EnumFieldAnalyzerState::get(),
          constant_propagation::BoxedBooleanAnalyzerState::get(), nullptr));
  intra_cp.run(initial_env);
  for (const auto& block : cfg.blocks()) {
    auto env = intra_cp.get_entry_state_at(block);
    if (env.is_bottom()) {
      res.dead_blocks++;
      // we found an unreachable block; ignore invoke instructions in it
      continue;
    }
    auto last_insn = block->get_last_insn();
    auto iterable = InstructionIterable(block);
    for (auto it = iterable.begin(); it != iterable.end(); it++) {
      auto insn = it->insn;
      auto callee = m_get_callee_fn(caller, insn);
      if (callee && callees.count(callee)) {
        CallSiteSummary call_site_summary;
        const auto& srcs = insn->srcs();
        for (size_t i = is_static(callee) ? 0 : 1; i < srcs.size(); ++i) {
          auto val = env.get(srcs[i]);
          always_assert(!val.is_bottom());
          call_site_summary.arguments.set(i, val);
        }
        call_site_summary.result_used =
            !callee->get_proto()->is_void() &&
            !cfg.move_result_of(block->to_cfg_instruction_iterator(it))
                 .is_end();
        res.invoke_call_site_summaries.emplace_back(
            insn, internalize_call_site_summary(call_site_summary));
      }
      intra_cp.analyze_instruction(insn, &env, insn == last_insn->insn);
      if (env.is_bottom()) {
        // Can happen in the absence of throw edges when dereferencing null
        break;
      }
    }
  }

  return res;
}

const std::vector<CallSiteSummaryOccurrences>*
CallSiteSummarizer::get_callee_call_site_summary_occurrences(
    const DexMethod* callee) const {
  auto it = m_callee_call_site_summary_occurrences.find(callee);
  return it == m_callee_call_site_summary_occurrences.end() ? nullptr
                                                            : &it->second;
}

const std::vector<const IRInstruction*>*
CallSiteSummarizer::get_callee_call_site_invokes(
    const DexMethod* callee) const {
  auto it = m_callee_call_site_invokes.find(callee);
  return it == m_callee_call_site_invokes.end() ? nullptr : &it->second;
}

const CallSiteSummary* CallSiteSummarizer::get_instruction_call_site_summary(
    const IRInstruction* invoke_insn) const {
  auto it = m_invoke_call_site_summaries.find(invoke_insn);
  return it == m_invoke_call_site_summaries.end() ? nullptr : &*it->second;
}

} // namespace inliner
