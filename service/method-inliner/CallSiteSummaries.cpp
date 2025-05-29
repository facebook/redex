/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
    append_key_value(oss, value);
  }
  return oss.str();
}

static void append_key_value(std::ostringstream& oss,
                             const SignedConstantDomain& signed_value) {
  const auto c = signed_value.get_constant();
  if (c) {
    // prefer compact pretty value
    oss << *c;
  } else {
    oss << signed_value;
  }
}

static void append_key_value(std::ostringstream& oss,
                             const SingletonObjectDomain& singleton_value) {
  auto dex_field = singleton_value.get_constant();
  always_assert(dex_field);
  oss << show(*dex_field);
}

static void append_key_value(std::ostringstream& oss,
                             const StringDomain& string_value) {
  auto dex_string = string_value.get_constant();
  always_assert(dex_string);
  auto str = (*dex_string)->str();
  oss << std::quoted(str_copy(str));
}

static void append_key_value(std::ostringstream& oss,
                             const ObjectWithImmutAttrDomain& obj_or_none) {
  auto object = obj_or_none.get_constant();
  always_assert(object);
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
      oss << show(attr.attr.val.field);
    } else {
      always_assert(attr.attr.is_method());
      oss << show(attr.attr.val.method);
    }
    oss << "=";
    if (const auto& signed_value =
            attr.value.maybe_get<SignedConstantDomain>()) {
      append_key_value(oss, *signed_value);
    } else if (const auto& string_value =
                   attr.value.maybe_get<StringDomain>()) {
      append_key_value(oss, *string_value);
    } else {
      not_reached_log("unexpected value: %s", SHOW(attr.value));
    }
  }
  oss << "}";
}

static void append_key_value(std::ostringstream& oss,
                             const ConstantClassObjectDomain& class_or_none) {
  oss << "(class)";
  auto class_opt = class_or_none.get_constant();
  if (class_opt) {
    // the DexType* pointer is unique
    oss << "@" << *class_opt;
  }
}

static void append_key_value(std::ostringstream& oss,
                             const NewObjectDomain& new_obj_or_none) {
  oss << "(new-object)";
  auto type = new_obj_or_none.get_type();
  if (type) {
    oss << show(type);
  }
  auto new_object_insn = new_obj_or_none.get_new_object_insn();
  if (new_object_insn) {
    // the IRInstruction* pointer is unique
    oss << "@" << new_object_insn;
  }
  auto array_length = new_obj_or_none.get_array_length();
  always_assert(!array_length.is_bottom());
  if (!array_length.is_top()) {
    oss << "[";
    ::append_key_value(oss, array_length);
    oss << "]";
  }
}

void CallSiteSummary::append_key_value(std::ostringstream& oss,
                                       const ConstantValue& value) {
  if (const auto& signed_value = value.maybe_get<SignedConstantDomain>()) {
    ::append_key_value(oss, *signed_value);
  } else if (const auto& singleton_value =
                 value.maybe_get<SingletonObjectDomain>()) {
    ::append_key_value(oss, *singleton_value);
  } else if (const auto& obj_or_none =
                 value.maybe_get<ObjectWithImmutAttrDomain>()) {
    ::append_key_value(oss, *obj_or_none);
  } else if (const auto& string_value = value.maybe_get<StringDomain>()) {
    ::append_key_value(oss, *string_value);
  } else if (const auto& class_or_none =
                 value.maybe_get<ConstantClassObjectDomain>()) {
    ::append_key_value(oss, *class_or_none);
  } else if (const auto& new_obj_or_none = value.maybe_get<NewObjectDomain>()) {
    ::append_key_value(oss, *new_obj_or_none);
  } else {
    not_reached_log("unexpected value: %s", SHOW(value));
  }
}

namespace inliner {

CallSiteSummarizer::CallSiteSummarizer(
    shrinker::Shrinker& shrinker,
    const ConcurrentMethodToMethodOccurrences& callee_caller,
    const ConcurrentMethodToMethodOccurrences& caller_callee,
    GetCalleeFunction get_callee_fn,
    HasCalleeOtherCallSitesPredicate has_callee_other_call_sites_fn,
    std::function<bool(const ConstantValue&)>* filter_fn,
    CallSiteSummaryStats* stats)
    : m_shrinker(shrinker),
      m_callee_caller(callee_caller),
      m_caller_callee(caller_callee),
      m_get_callee_fn(std::move(get_callee_fn)),
      m_has_callee_other_call_sites_fn(
          std::move(has_callee_other_call_sites_fn)),
      m_filter_fn(filter_fn),
      m_stats(stats) {}

const CallSiteSummary* CallSiteSummarizer::internalize_call_site_summary(
    const CallSiteSummary& call_site_summary) {
  return m_call_site_summaries
      .get_or_emplace_and_assert_equal(call_site_summary.get_key(),
                                       call_site_summary)
      .first;
}

void CallSiteSummarizer::summarize() {
  Timer t("compute_call_site_summaries");

  // We'll do a top-down traversal of all call-sites, in order to propagate
  // call-site information from outer call-sites to nested call-sites, improving
  // the precision of the analysis. This is effectively an inter-procedural
  // constant-propagation analysis, but we are operating on a reduced call-graph
  // as recursion has been broken by eliminating some call-sites from
  // consideration, and we are only considering those methods that are involved
  // in an inlinable caller-callee relationship, which excludes much of true
  // virtual methods.
  PriorityThreadPoolDAGScheduler<DexMethod*> summaries_scheduler;

  // Helper function to retrieve a list of callers of a callee such that all
  // possible call-sites to the callee are in the returned callers.
  auto get_dependencies =
      [&](DexMethod* callee) -> const UnorderedMap<DexMethod*, size_t>* {
    const auto* ptr = m_callee_caller.get_unsafe(callee);
    if (ptr == nullptr || m_has_callee_other_call_sites_fn(callee)) {
      return nullptr;
    }
    // If we get here, then we know all possible call-sites to the callee, and
    // they reside in the known list of callers.
    return ptr;
  };

  summaries_scheduler.set_executor([&](DexMethod* method) {
    CallSiteArguments arguments;
    if (!get_dependencies(method)) {
      // There are no relevant callers from which we could gather incoming
      // constant arguments.
      arguments = CallSiteArguments::top();
    } else {
      CalleeInfo* ci = nullptr;
      auto success =
          m_callee_infos.observe(method, [&](auto*, const auto& val) {
            ci = const_cast<CalleeInfo*>(&val);
          });
      always_assert(success == !!ci);
      if (ci == nullptr) {
        // All callers were unreachable
        arguments = CallSiteArguments::bottom();
      } else {
        // Release memory for indices
        ci->indices.clear();
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
    auto& callees = m_caller_callee.at_unsafe(method);
    ConstantEnvironment initial_env =
        constant_propagation::interprocedural::env_with_params(
            is_static(method), method->get_code(), arguments);
    auto res = get_invoke_call_site_summaries(method, callees, initial_env);
    for (auto& p : res.invoke_call_site_summaries) {
      auto insn = p.first;
      auto callee = m_get_callee_fn(method, insn);
      auto call_site_summary = p.second;
      m_callee_infos.update(
          callee, [&](const DexMethod*, CalleeInfo& ci, bool /* exists */) {
            auto [it, emplaced] =
                ci.indices.emplace(call_site_summary, ci.indices.size());
            if (emplaced) {
              ci.occurrences.emplace_back(call_site_summary, 1);
            } else {
              ci.occurrences[it->second].second++;
            }
            ci.invokes.push_back(insn);
          });
      m_invoke_call_site_summaries.emplace(insn, call_site_summary);
    }
    m_stats->constant_invoke_callers_analyzed++;
    if (res.dead_blocks > 0) {
      m_stats->constant_invoke_callers_unreachable_blocks += res.dead_blocks;
    }
  });

  std::vector<DexMethod*> callers;
  callers.reserve(m_caller_callee.size());
  for (auto& p : UnorderedIterable(m_caller_callee)) {
    auto method = const_cast<DexMethod*>(p.first);
    callers.push_back(method);
    auto dependencies = get_dependencies(method);
    if (dependencies) {
      for (auto& q : UnorderedIterable(*dependencies)) {
        summaries_scheduler.add_dependency(method, q.first);
      }
    }
  }
  m_stats->constant_invoke_callers_critical_path_length =
      summaries_scheduler.run(std::move(callers));
}

InvokeCallSiteSummariesAndDeadBlocks
CallSiteSummarizer::get_invoke_call_site_summaries(
    DexMethod* caller,
    const UnorderedMap<DexMethod*, size_t>& callees,
    const ConstantEnvironment& initial_env) {
  IRCode* code = caller->get_code();

  InvokeCallSiteSummariesAndDeadBlocks res;
  auto& cfg = code->cfg();
  constant_propagation::intraprocedural::FixpointIterator intra_cp(
      &m_shrinker.get_cp_state(),
      cfg,
      constant_propagation::ConstantPrimitiveAndBoxedAnalyzer(
          m_shrinker.get_immut_analyzer_state(),
          m_shrinker.get_immut_analyzer_state(),
          constant_propagation::EnumFieldAnalyzerState::get(),
          constant_propagation::BoxedBooleanAnalyzerState::get(),
          m_shrinker.get_string_analyzer_state(),
          constant_propagation::ApiLevelAnalyzerState::get(),
          m_shrinker.get_package_name_state(), nullptr,
          m_shrinker.get_immut_analyzer_state(), nullptr));
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
          if (val.is_top()) {
            continue;
          }
          if (m_filter_fn && !(*m_filter_fn)(val)) {
            continue;
          }
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
  auto ptr = m_callee_infos.get_unsafe(callee);
  return ptr == nullptr ? nullptr : &ptr->occurrences;
}

const std::vector<const IRInstruction*>*
CallSiteSummarizer::get_callee_call_site_invokes(
    const DexMethod* callee) const {
  auto ptr = m_callee_infos.get_unsafe(callee);
  return ptr == nullptr ? nullptr : &ptr->invokes;
}

const CallSiteSummary* CallSiteSummarizer::get_instruction_call_site_summary(
    const IRInstruction* invoke_insn) const {
  auto ptr = m_invoke_call_site_summaries.get_unsafe(invoke_insn);
  return ptr == nullptr ? nullptr : *ptr;
}

} // namespace inliner
