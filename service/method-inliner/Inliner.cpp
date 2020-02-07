/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Inliner.h"

#include <utility>

#include "ApiLevelChecker.h"
#include "CFGInliner.h"
#include "ConcurrentContainers.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationWholeProgramState.h"
#include "ConstructorAnalysis.h"
#include "ControlFlow.h"
#include "DexUtil.h"
#include "EditableCfgAdapter.h"
#include "IRInstruction.h"
#include "InlineForSpeed.h"
#include "LocalDce.h"
#include "MethodProfiles.h"
#include "Mutators.h"
#include "OptData.h"
#include "Purity.h"
#include "Resolver.h"
#include "Timer.h"
#include "Transform.h"
#include "UnknownVirtuals.h"
#include "Walkers.h"
#include "WorkQueue.h"

using namespace opt_metadata;

namespace {

// The following costs are in terms of code-units (2 bytes).

// Inlining methods that belong to different classes might lead to worse
// cross-dex-ref minimization results. We account for this.
const size_t COST_INTER_DEX_SOME_CALLERS_DIFFERENT_CLASSES = 2;

// Typical overhead of calling a method with a result. This isn't just the
// overhead of the invoke instruction itself, but possibly some setup and
// consumption of result.
const size_t COST_INVOKE_WITH_RESULT = 5;

// Typical overhead of calling a method without a result.
const size_t COST_INVOKE_WITHOUT_RESULT = 3;

// Overhead of having a method and its metadata.
const size_t COST_METHOD = 16;

// When to consider running constant-propagation to better estimate inlined
// cost. It just takes too much time to run the analysis for large methods.
const size_t MAX_COST_FOR_CONSTANT_PROPAGATION = 272;

// Minimum number of instructions needed across all constant arguments
// variations before parallelizing constant-propagation.
const size_t MIN_COST_FOR_PARALLELIZATION = 1977;

/*
 * This is the maximum size of method that Dex bytecode can encode.
 * The table of instructions is indexed by a 32 bit unsigned integer.
 */
constexpr uint64_t HARD_MAX_INSTRUCTION_SIZE = 1L << 32;

/*
 * Some versions of ART (5.0.0 - 5.0.2) will fail to verify a method if it
 * is too large. See https://code.google.com/p/android/issues/detail?id=66655.
 *
 * The verifier rounds up to the next power of two, and doesn't support any
 * size greater than 16. See
 * http://androidxref.com/5.0.0_r2/xref/art/compiler/dex/verified_method.cc#107
 */
constexpr uint32_t SOFT_MAX_INSTRUCTION_SIZE = 1 << 15;
constexpr uint32_t INSTRUCTION_BUFFER = 1 << 12;

} // namespace

MultiMethodInliner::MultiMethodInliner(
    const std::vector<DexClass*>& scope,
    DexStoresVector& stores,
    const std::unordered_set<DexMethod*>& candidates,
    std::function<DexMethod*(DexMethodRef*, MethodSearch)> resolve_fn,
    const inliner::InlinerConfig& config,
    MultiMethodInlinerMode mode /* default is InterDex */,
    const CalleeCallerInsns& true_virtual_callers,
    const std::unordered_map<const DexMethodRef*, method_profiles::Stats>&
        method_profile_stats,
    const std::unordered_map<const DexMethod*, size_t>&
        same_method_implementations,
    bool analyze_and_prune_inits)
    : resolver(std::move(resolve_fn)),
      xstores(stores),
      m_scope(scope),
      m_config(config),
      m_mode(mode),
      m_hot_methods(
          inline_for_speed::compute_hot_methods(method_profile_stats)),
      m_same_method_implementations(same_method_implementations),
      m_pure_methods(get_pure_methods()),
      m_analyze_and_prune_inits(analyze_and_prune_inits) {
  for (const auto& callee_callers : true_virtual_callers) {
    for (const auto& caller_insns : callee_callers.second) {
      for (auto insn : caller_insns.second) {
        caller_virtual_callee[caller_insns.first][insn] = callee_callers.first;
      }
    }
  }
  // Walk every opcode in scope looking for calls to inlinable candidates and
  // build a map of callers to callees and the reverse callees to callers. If
  // intra_dex is false, we build the map for all the candidates. If intra_dex
  // is true, we properly exclude methods who have callers being located in
  // another dex from the candidates.
  if (mode == IntraDex) {
    std::unordered_set<DexMethod*> candidate_callees(candidates.begin(),
                                                     candidates.end());
    XDexRefs x_dex(stores);
    walk::opcodes(scope, [](DexMethod* caller) { return true; },
                  [&](DexMethod* caller, IRInstruction* insn) {
                    if (is_invoke(insn->opcode())) {
                      auto callee =
                          resolver(insn->get_method(), opcode_to_search(insn));
                      if (callee != nullptr && callee->is_concrete() &&
                          candidate_callees.count(callee) &&
                          true_virtual_callers.count(callee) == 0) {
                        if (x_dex.cross_dex_ref(caller, callee)) {
                          candidate_callees.erase(callee);
                          if (callee_caller.count(callee)) {
                            callee_caller.erase(callee);
                          }
                        } else {
                          callee_caller[callee].push_back(caller);
                        }
                      }
                    }
                  });
    for (const auto& callee_callers : true_virtual_callers) {
      auto callee = callee_callers.first;
      for (const auto& caller_insns : callee_callers.second) {
        auto caller = caller_insns.first;
        if (x_dex.cross_dex_ref(callee, caller)) {
          callee_caller.erase(callee);
          break;
        }
        callee_caller[callee].push_back(caller);
      }
    }
    for (auto& pair : callee_caller) {
      DexMethod* callee = const_cast<DexMethod*>(pair.first);
      const auto& callers = pair.second;
      for (auto caller : callers) {
        caller_callee[caller].push_back(callee);
      }
    }
  } else if (mode == InterDex) {
    walk::opcodes(
        scope, [](DexMethod* caller) { return true; },
        [&](DexMethod* caller, IRInstruction* insn) {
          if (is_invoke(insn->opcode())) {
            auto callee = resolver(insn->get_method(), opcode_to_search(insn));
            if (true_virtual_callers.count(callee) == 0 && callee != nullptr &&
                callee->is_concrete() && candidates.count(callee)) {
              callee_caller[callee].push_back(caller);
              caller_callee[caller].push_back(callee);
            }
          }
        });
    for (const auto& callee_callers : true_virtual_callers) {
      auto callee = callee_callers.first;
      for (const auto& caller_insns : callee_callers.second) {
        auto caller = caller_insns.first;
        callee_caller[callee].push_back(caller);
        caller_callee[caller].push_back(callee);
      }
    }
  }

  m_shrinking_enabled = m_config.run_const_prop || m_config.run_cse ||
                        m_config.run_copy_prop || m_config.run_local_dce ||
                        m_config.run_dedup_blocks;

  if (config.run_cse) {
    m_cse_shared_state =
        std::make_unique<cse_impl::SharedState>(m_pure_methods);
  }
}

/*
 * The key of a constant-arguments data structure is a string representation
 * that approximates the constant arguments.
 */
static std::string get_key(const ConstantArguments& constant_arguments) {
  always_assert(!constant_arguments.is_bottom());
  if (constant_arguments.is_top()) {
    return "";
  }
  std::ostringstream oss;
  bool first = true;
  for (auto& p : constant_arguments.bindings()) {
    if (first) {
      first = false;
    } else {
      oss << ",";
    }
    oss << p.first << ":";
    // TODO: Is it worthwhile to distinguish other domain cases?
    auto c = p.second.maybe_get<SignedConstantDomain>();
    if (c && c->get_constant()) {
      oss << *c->get_constant();
    }
  }
  return oss.str();
}

void MultiMethodInliner::compute_callee_constant_arguments() {
  if (!m_config.use_constant_propagation_for_callee_size) {
    return;
  }

  Timer t("compute_callee_constant_arguments");
  struct CalleeInfo {
    std::unordered_map<std::string, ConstantArguments> constant_arguments;
    std::unordered_map<std::string, size_t> occurrences;
  };
  ConcurrentMap<DexMethod*, CalleeInfo> concurrent_callee_constant_arguments;
  auto wq = workqueue_foreach<DexMethod*>(
      [&](DexMethod* caller) {
        auto& callees = caller_callee.at(caller);
        auto res = get_invoke_constant_arguments(caller, callees);
        if (!res) {
          return;
        }
        for (auto& p : res->invoke_constant_arguments) {
          auto insn = p.first->insn;
          auto callee = resolver(insn->get_method(), opcode_to_search(insn));
          const auto& constant_arguments = p.second;
          auto key = get_key(constant_arguments);
          concurrent_callee_constant_arguments.update(
              callee, [&](const DexMethod*, CalleeInfo& ci, bool /* exists */) {
                ci.constant_arguments.emplace(key, constant_arguments);
                ++ci.occurrences[key];
              });
        }
        info.constant_invoke_callers_analyzed++;
        info.constant_invoke_callers_unreachable_blocks += res->dead_blocks;
      },
      redex_parallel::default_num_threads());
  for (auto& p : caller_callee) {
    wq.add_item(p.first);
  };
  wq.run_all();
  for (auto& p : concurrent_callee_constant_arguments) {
    auto& v = m_callee_constant_arguments[p.first];
    const auto& ci = p.second;
    for (const auto& q : ci.occurrences) {
      const auto& key = q.first;
      const auto& constant_arguments = q.second;
      v.emplace_back(ci.constant_arguments.at(key), constant_arguments);
    }
  }
}

void MultiMethodInliner::inline_methods() {
  compute_callee_constant_arguments();

  // Inlining and shrinking initiated from within this method will be done
  // in parallel.
  m_async_method_executor.set_num_threads(
      m_config.debug ? 1 : redex_parallel::default_num_threads());

  // The order in which we inline is such that once a callee is considered to
  // be inlined, it's code will no longer change. So we can cache its size.
  m_callee_insn_sizes =
      std::make_unique<ConcurrentMap<const DexMethod*, size_t>>();

  // Instead of changing visibility as we inline, blocking other work on the
  // critical path, we do it all in parallel at the end.
  m_delayed_change_visibilities = std::make_unique<
      ConcurrentMap<DexMethod*, std::unordered_set<DexType*>>>();

  // we want to inline bottom up, so as a first step we identify all the
  // top level callers, then we recurse into all inlinable callees until we
  // hit a leaf and we start inlining from there.
  // First, we just gather data on caller/non-recursive-callees pairs for each
  // stack depth.
  std::unordered_map<DexMethod*, size_t> visited;
  CallerNonrecursiveCalleesByStackDepth
      caller_nonrecursive_callees_by_stack_depth;
  for (const auto& it : caller_callee) {
    auto caller = it.first;
    TraceContext context(caller->get_deobfuscated_name());
    // if the caller is not a top level keep going, it will be traversed
    // when inlining a top level caller
    if (callee_caller.find(caller) != callee_caller.end()) continue;
    sparta::PatriciaTreeSet<DexMethod*> call_stack;
    auto stack_depth = compute_caller_nonrecursive_callees_by_stack_depth(
        caller, it.second, call_stack, &visited,
        &caller_nonrecursive_callees_by_stack_depth);
    info.max_call_stack_depth =
        std::max(info.max_call_stack_depth, stack_depth);
  }

  std::vector<size_t> ordered_stack_depths;
  for (auto& p : caller_nonrecursive_callees_by_stack_depth) {
    ordered_stack_depths.push_back(p.first);
  }
  std::sort(ordered_stack_depths.begin(), ordered_stack_depths.end());

  // Second, compute caller priorities --- the callers get a priority assigned
  // that reflects how many other callers will be waiting for them.
  // We also compute the set of callers and some other auxiliary data structures
  // along the way.
  // TODO: Instead of just considering the length of the critical path as the
  // priority, consider some variations, e.g. include the fan-out of the
  // dependencies divided by the number of threads.
  for (int i = ((int)ordered_stack_depths.size()) - 1; i >= 0; i--) {
    auto stack_depth = ordered_stack_depths.at(i);
    auto& caller_nonrecursive_callees =
        caller_nonrecursive_callees_by_stack_depth.at(stack_depth);
    for (auto& p : caller_nonrecursive_callees) {
      auto caller = p.first;
      auto& callees = p.second;
      always_assert(callees.size() > 0);
      int caller_priority = 0;
      auto method_priorities_it = m_async_callee_priorities.find(caller);
      if (method_priorities_it != m_async_callee_priorities.end()) {
        caller_priority = method_priorities_it->second;
      }
      for (auto callee : callees) {
        auto& callee_priority = m_async_callee_priorities[callee];
        callee_priority = std::max(callee_priority, caller_priority + 1);
        m_async_callee_callers[callee].push_back(caller);
      }
      m_async_caller_wait_counts.emplace(caller, callees.size());
      m_async_caller_callees.emplace(caller, callees);
    }
  }
  for (auto& p : m_async_callee_callers) {
    auto callee = p.first;
    auto& callers = p.second;
    auto& callee_priority = m_async_callee_priorities[callee];
    info.critical_path_length =
        std::max(info.critical_path_length, callee_priority);
    callee_priority = (callee_priority << 16) + callers.size();
  }

  // Kick off (shrinking and) pre-computing the should-inline cache.
  // Once all callees of a caller have been processed, then postprocessing
  // will in turn kick off processing of the caller.
  for (auto& p : m_async_callee_priorities) {
    auto callee = p.first;
    auto priority = p.second;
    always_assert(priority > 0);
    if (m_async_caller_callees.count(callee) == 0) {
      async_postprocess_method(const_cast<DexMethod*>(callee));
    }
  }

  if (m_shrinking_enabled && m_config.shrink_other_methods) {
    walk::code(m_scope, [&](DexMethod* method, IRCode& code) {
      // If a method is not tracked as a caller, and not already in the
      // processing pool because it's a callee, then process it.
      if (m_async_caller_callees.count(method) == 0 &&
          m_async_callee_priorities.count(method) == 0) {
        async_postprocess_method(const_cast<DexMethod*>(method));
      }
    });
  }

  m_async_method_executor.join();
  delayed_change_visibilities();
  info.waited_seconds = m_async_method_executor.get_waited_seconds();
}

size_t MultiMethodInliner::compute_caller_nonrecursive_callees_by_stack_depth(
    DexMethod* caller,
    const std::vector<DexMethod*>& callees,
    sparta::PatriciaTreeSet<DexMethod*> call_stack,
    std::unordered_map<DexMethod*, size_t>* visited,
    CallerNonrecursiveCalleesByStackDepth*
        caller_nonrecursive_callees_by_stack_depth) {
  always_assert(callees.size() > 0);

  auto visited_it = visited->find(caller);
  if (visited_it != visited->end()) {
    return visited_it->second;
  }

  // We'll only know the exact call stack depth at the end.
  visited->emplace(caller, std::numeric_limits<size_t>::max());
  call_stack.insert(caller);

  std::vector<DexMethod*> nonrecursive_callees;
  nonrecursive_callees.reserve(callees.size());
  size_t stack_depth = 0;
  // recurse into the callees in case they have something to inline on
  // their own. We want to inline bottom up so that a callee is
  // completely resolved by the time it is inlined.
  for (auto callee : callees) {
    if (call_stack.contains(callee)) {
      // we've found recursion in the current call stack
      always_assert(visited->at(callee) == std::numeric_limits<size_t>::max());
      info.recursive++;
      continue;
    }
    size_t callee_stack_depth = 0;
    auto maybe_caller = caller_callee.find(callee);
    if (maybe_caller != caller_callee.end()) {
      callee_stack_depth = compute_caller_nonrecursive_callees_by_stack_depth(
          callee, maybe_caller->second, call_stack, visited,
          caller_nonrecursive_callees_by_stack_depth);
    }

    if (!for_speed()) {
      nonrecursive_callees.push_back(callee);
    } else if (inline_for_speed::should_inline(caller, callee, m_hot_methods)) {
      TRACE(METH_PROF, 3, "%s, %s", SHOW(caller), SHOW(callee));
      nonrecursive_callees.push_back(callee);
    }

    stack_depth = std::max(stack_depth, callee_stack_depth + 1);
  }

  (*visited)[caller] = stack_depth;
  if (nonrecursive_callees.size() > 0) {
    (*caller_nonrecursive_callees_by_stack_depth)[stack_depth].push_back(
        std::make_pair(caller, nonrecursive_callees));
  }
  return stack_depth;
}

void MultiMethodInliner::caller_inline(
    DexMethod* caller, const std::vector<DexMethod*>& nonrecursive_callees) {
  // We select callees to inline into this caller
  std::vector<DexMethod*> selected_callees;
  selected_callees.reserve(nonrecursive_callees.size());
  for (auto callee : nonrecursive_callees) {
    if (should_inline(callee)) {
      selected_callees.push_back(callee);
    }
  }

  if (selected_callees.size() > 0) {
    inline_callees(caller, selected_callees);
  }
}

boost::optional<InvokeConstantArgumentsAndDeadBlocks>
MultiMethodInliner::get_invoke_constant_arguments(
    DexMethod* caller, const std::vector<DexMethod*>& callees) {
  IRCode* code = caller->get_code();
  if (!code->editable_cfg_built()) {
    return boost::none;
  }

  InvokeConstantArgumentsAndDeadBlocks res;
  std::unordered_set<DexMethod*> callees_set(callees.begin(), callees.end());
  auto& cfg = code->cfg();
  constant_propagation::intraprocedural::FixpointIterator intra_cp(
      cfg, constant_propagation::ConstantPrimitiveAnalyzer());
  intra_cp.run(ConstantEnvironment());
  for (const auto& block : cfg.blocks()) {
    auto env = intra_cp.get_entry_state_at(block);
    if (env.is_bottom()) {
      res.dead_blocks++;
      // we found an unreachable block; ignore invoke instructions in it
      continue;
    }
    for (auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      if (is_invoke(insn->opcode())) {
        auto callee = resolver(insn->get_method(), opcode_to_search(insn));
        if (callees_set.count(callee)) {
          ConstantArguments constant_arguments;
          const auto& srcs = insn->srcs();
          for (size_t i = 0; i < srcs.size(); ++i) {
            auto val = env.get(srcs[i]);
            always_assert(!val.is_bottom());
            constant_arguments.set(i, val);
          }
          res.invoke_constant_arguments.emplace_back(code->iterator_to(mie),
                                                     constant_arguments);
        }
      }
      intra_cp.analyze_instruction(insn, &env);
    }
  }

  return res;
}

void MultiMethodInliner::inline_callees(
    DexMethod* caller, const std::vector<DexMethod*>& callees) {
  size_t found = 0;

  // walk the caller opcodes collecting all candidates to inline
  // Build a callee to opcode map
  std::vector<std::pair<DexMethod*, IRList::iterator>> inlinables;
  editable_cfg_adapter::iterate_with_iterator(
      caller->get_code(), [&](const IRList::iterator& it) {
        auto insn = it->insn;
        if (!is_invoke(insn->opcode())) {
          return editable_cfg_adapter::LOOP_CONTINUE;
        }
        auto callee = resolver(insn->get_method(), opcode_to_search(insn));
        if (caller_virtual_callee.count(caller) &&
            caller_virtual_callee[caller].count(insn)) {
          callee = caller_virtual_callee[caller][insn];
          always_assert(callee);
        }
        if (callee == nullptr) {
          return editable_cfg_adapter::LOOP_CONTINUE;
        }
        if (std::find(callees.begin(), callees.end(), callee) ==
            callees.end()) {
          return editable_cfg_adapter::LOOP_CONTINUE;
        }
        always_assert(callee->is_concrete());
        if (m_analyze_and_prune_inits && method::is_init(callee)) {
          if (!callee->get_code()->editable_cfg_built()) {
            return editable_cfg_adapter::LOOP_CONTINUE;
          }
          if (!can_inline_init(callee)) {
            if (!method::is_init(caller) ||
                caller->get_class() != callee->get_class() ||
                !caller->get_code()->editable_cfg_built() ||
                !constructor_analysis::can_inline_inits_in_same_class(
                    caller, callee, insn)) {
              return editable_cfg_adapter::LOOP_CONTINUE;
            }
          }
        }
        found++;
        inlinables.emplace_back(callee, it);
        if (found == callees.size()) {
          return editable_cfg_adapter::LOOP_BREAK;
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
  if (found != callees.size()) {
    always_assert(found <= callees.size());
    info.not_found += callees.size() - found;
  }

  if (inlinables.size() > 0) {
    inline_inlinables(caller, inlinables);
  }
}

void MultiMethodInliner::inline_callees(
    DexMethod* caller, const std::unordered_set<IRInstruction*>& insns) {
  std::vector<std::pair<DexMethod*, IRList::iterator>> inlinables;
  editable_cfg_adapter::iterate_with_iterator(
      caller->get_code(), [&](const IRList::iterator& it) {
        auto insn = it->insn;
        if (insns.count(insn)) {
          auto callee = resolver(insn->get_method(), opcode_to_search(insn));
          if (caller_virtual_callee.count(caller) &&
              caller_virtual_callee[caller].count(insn)) {
            callee = caller_virtual_callee[caller][insn];
          }
          if (callee == nullptr) {
            return editable_cfg_adapter::LOOP_CONTINUE;
          }
          always_assert(callee->is_concrete());
          inlinables.emplace_back(callee, it);
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });

  inline_inlinables(caller, inlinables);
}

bool MultiMethodInliner::inline_inlinables_need_deconstruct(DexMethod* method) {
  // The mixed CFG, IRCode is used by Switch Inline (only?) where the caller is
  // an IRCode and the callee is a CFG.
  return m_config.use_cfg_inliner && !method->get_code()->editable_cfg_built();
}

void MultiMethodInliner::inline_inlinables(
    DexMethod* caller_method,
    const std::vector<std::pair<DexMethod*, IRList::iterator>>& inlinables) {

  auto caller = caller_method->get_code();
  std::unordered_set<IRCode*> need_deconstruct;
  if (inline_inlinables_need_deconstruct(caller_method)) {
    need_deconstruct.reserve(1 + inlinables.size());
    need_deconstruct.insert(caller);
    for (const auto& inlinable : inlinables) {
      need_deconstruct.insert(inlinable.first->get_code());
    }
    for (auto code : need_deconstruct) {
      always_assert(!code->editable_cfg_built());
      code->build_cfg(/* editable */ true);
    }
  }

  // attempt to inline all inlinable candidates
  size_t estimated_insn_size = caller->editable_cfg_built()
                                   ? caller->cfg().sum_opcode_sizes()
                                   : caller->sum_opcode_sizes();

  // Prefer inlining smaller methods first, so that we are less likely to hit
  // overall size limit.
  std::vector<std::pair<DexMethod*, IRList::iterator>> ordered_inlinables(
      inlinables.begin(), inlinables.end());

  std::stable_sort(ordered_inlinables.begin(), ordered_inlinables.end(),
                   [this](const std::pair<DexMethod*, IRList::iterator>& a,
                          const std::pair<DexMethod*, IRList::iterator>& b) {
                     return get_callee_insn_size(a.first) <
                            get_callee_insn_size(b.first);
                   });

  std::vector<DexMethod*> inlined_callees;
  for (const auto& inlinable : ordered_inlinables) {
    auto callee_method = inlinable.first;
    auto callee = callee_method->get_code();
    auto callsite = inlinable.second;

    if (!is_inlinable(caller_method, callee_method, callsite->insn,
                      estimated_insn_size)) {
      continue;
    }

    TRACE(MMINL, 4, "inline %s (%d) in %s (%d)", SHOW(callee),
          caller->get_registers_size(), SHOW(caller),
          callee->get_registers_size());

    if (m_config.use_cfg_inliner) {
      bool success = inliner::inline_with_cfg(caller_method, callee_method,
                                              callsite->insn);
      if (!success) {
        continue;
      }
    } else {
      // Logging before the call to inline_method to get the most relevant line
      // number near callsite before callsite gets replaced. Should be ok as
      // inline_method does not fail to inline.
      log_opt(INLINED, caller_method, callsite->insn);

      inliner::inline_method_unsafe(caller, callee, callsite);
    }
    TRACE(INL, 2, "caller: %s\tcallee: %s", SHOW(caller), SHOW(callee));
    estimated_insn_size += get_callee_insn_size(callee_method);

    inlined_callees.push_back(callee_method);
  }

  if (inlined_callees.size() > 0) {
    for (auto callee_method : inlined_callees) {
      if (m_delayed_change_visibilities) {
        m_delayed_change_visibilities->update(
            callee_method, [caller_method](const DexMethod*,
                                           std::unordered_set<DexType*>& value,
                                           bool /*exists*/) {
              value.insert(caller_method->get_class());
            });
      } else {
        std::lock_guard<std::mutex> guard(m_change_visibility_mutex);
        change_visibility(callee_method, caller_method->get_class());
      }
      m_inlined.insert(callee_method);
    }
  }

  for (IRCode* code : need_deconstruct) {
    code->clear_cfg();
  }

  info.calls_inlined += inlined_callees.size();
}

void MultiMethodInliner::async_prioritized_method_execute(
    DexMethod* method, const std::function<void()>& f) {
  int priority = std::numeric_limits<int>::min();
  auto it = m_async_callee_priorities.find(method);
  if (it != m_async_callee_priorities.end()) {
    priority = it->second;
  }
  m_async_method_executor.post(priority, f);
}

void MultiMethodInliner::async_postprocess_method(DexMethod* method) {
  if (m_async_callee_priorities.count(method) == 0 &&
      (!m_shrinking_enabled || method->rstate.no_optimizations())) {
    return;
  }

  async_prioritized_method_execute(
      method, [method, this]() { postprocess_method(method); });
}

void MultiMethodInliner::shrink_method(DexMethod* method) {
  auto code = method->get_code();
  bool editable_cfg_built = code->editable_cfg_built();

  constant_propagation::Transform::Stats const_prop_stats;
  cse_impl::Stats cse_stats;
  copy_propagation_impl::Stats copy_prop_stats;
  LocalDce::Stats local_dce_stats;
  dedup_blocks_impl::Stats dedup_blocks_stats;

  if (m_config.run_const_prop) {
    if (editable_cfg_built) {
      code->clear_cfg();
    }
    if (!code->cfg_built()) {
      code->build_cfg(/* editable */ false);
    }
    constant_propagation::intraprocedural::FixpointIterator fp_iter(
        code->cfg(), constant_propagation::ConstantPrimitiveAnalyzer());
    fp_iter.run(ConstantEnvironment());
    constant_propagation::Transform::Config config;
    constant_propagation::Transform tf(config);
    const_prop_stats =
        tf.apply(fp_iter, constant_propagation::WholeProgramState(), code);
  }

  // The following default 'features' of copy propagation would only
  // interfere with what CSE is trying to do.
  copy_propagation_impl::Config copy_prop_config;
  copy_prop_config.eliminate_const_classes = false;
  copy_prop_config.eliminate_const_strings = false;
  copy_prop_config.static_finals = false;

  if (m_config.run_cse) {
    if (!code->editable_cfg_built()) {
      code->build_cfg(/* editable */ true);
    }

    cse_impl::CommonSubexpressionElimination cse(m_cse_shared_state.get(),
                                                 code->cfg());
    cse.patch(is_static(method), method->get_class(),
              method->get_proto()->get_args(),
              copy_prop_config.max_estimated_registers);
    cse_stats = cse.get_stats();
  }

  if (m_config.run_copy_prop) {
    if (code->editable_cfg_built()) {
      code->clear_cfg();
    }

    copy_propagation_impl::Config config;
    copy_propagation_impl::CopyPropagation copy_propagation(config);
    copy_prop_stats = copy_propagation.run(code, method);
  }

  if (m_config.run_local_dce) {
    // LocalDce doesn't care if editable_cfg_built
    auto local_dce = LocalDce(m_pure_methods);
    local_dce.dce(code);
    local_dce_stats = local_dce.get_stats();
  }

  if (m_config.run_dedup_blocks) {
    if (!code->editable_cfg_built()) {
      code->build_cfg(/* editable */ true);
    }

    dedup_blocks_impl::Config config;
    dedup_blocks_impl::DedupBlocks dedup_blocks(config, method);
    dedup_blocks.run();
    dedup_blocks_stats = dedup_blocks.get_stats();
  }

  if (editable_cfg_built && !code->editable_cfg_built()) {
    code->build_cfg(/* editable */ true);
  } else if (!editable_cfg_built && code->editable_cfg_built()) {
    code->clear_cfg();
  }

  std::lock_guard<std::mutex> guard(m_stats_mutex);
  m_const_prop_stats += const_prop_stats;
  m_cse_stats += cse_stats;
  m_copy_prop_stats += copy_prop_stats;
  m_local_dce_stats += local_dce_stats;
  m_dedup_blocks_stats += dedup_blocks_stats;
  m_methods_shrunk++;
}

void MultiMethodInliner::postprocess_method(DexMethod* method) {
  bool delayed_shrinking = false;
  bool is_callee = !!m_async_callee_priorities.count(method);
  if (m_shrinking_enabled && !method->rstate.no_optimizations()) {
    if (is_callee && should_inline_fast(method)) {
      // We know now that this method will get inlined regardless of the size
      // of its code. Therefore, we can delay shrinking, to unblock further
      // work more quickly.
      delayed_shrinking = true;
    } else {
      shrink_method(method);
    }
  }

  if (!is_callee) {
    // This method isn't the callee of another caller, so we can stop here.
    always_assert(!delayed_shrinking);
    return;
  }

  // This pre-populates the m_should_inline and m_callee_insn_sizes caches.
  if (should_inline(method)) {
    get_callee_insn_size(method);
  }

  auto& callers = m_async_callee_callers.at(method);
  if (delayed_shrinking) {
    m_async_delayed_shrinking_callee_wait_counts.emplace(method,
                                                         callers.size());
  }
  decrement_caller_wait_counts(callers);
}

void MultiMethodInliner::decrement_caller_wait_counts(
    const std::vector<DexMethod*>& callers) {
  for (auto caller : callers) {
    bool caller_ready = false;
    m_async_caller_wait_counts.update(
        caller, [&](const DexMethod*, size_t& value, bool /*exists*/) {
          caller_ready = --value == 0;
        });
    if (caller_ready) {
      if (inline_inlinables_need_deconstruct(caller)) {
        // TODO: Support parallel execution without pre-deconstructed cfgs.
        auto& callees = m_async_caller_callees.at(caller);
        caller_inline(caller, callees);
        decrement_delayed_shrinking_callee_wait_counts(callees);
        async_postprocess_method(caller);
      } else {
        // We can process inlining concurrently!
        async_prioritized_method_execute(caller, [caller, this]() {
          auto& callees = m_async_caller_callees.at(caller);
          caller_inline(caller, callees);
          decrement_delayed_shrinking_callee_wait_counts(callees);
          if (m_shrinking_enabled ||
              m_async_callee_priorities.count(caller) != 0) {
            postprocess_method(caller);
          }
        });
      }
    }
  }
}

void MultiMethodInliner::decrement_delayed_shrinking_callee_wait_counts(
    const std::vector<DexMethod*>& callees) {
  for (auto callee : callees) {
    if (!m_async_delayed_shrinking_callee_wait_counts.count(callee)) {
      continue;
    }

    bool callee_ready = false;
    m_async_delayed_shrinking_callee_wait_counts.update(
        callee, [&](const DexMethod*, size_t& value, bool /*exists*/) {
          callee_ready = --value == 0;
        });
    if (callee_ready) {
      int priority = std::numeric_limits<int>::min();
      m_async_method_executor.post(priority,
                                   [callee, this]() { shrink_method(callee); });
    }
  }
}

/**
 * Defines the set of rules that determine whether a function is inlinable.
 */
bool MultiMethodInliner::is_inlinable(DexMethod* caller,
                                      DexMethod* callee,
                                      const IRInstruction* insn,
                                      size_t estimated_insn_size) {
  // don't inline cross store references
  if (cross_store_reference(caller, callee)) {
    if (insn) {
      log_nopt(INL_CROSS_STORE_REFS, caller, insn);
    }
    return false;
  }
  if (is_blacklisted(callee)) {
    if (insn) {
      log_nopt(INL_BLACKLISTED_CALLEE, callee);
    }
    return false;
  }
  if (caller_is_blacklisted(caller)) {
    if (insn) {
      log_nopt(INL_BLACKLISTED_CALLER, caller);
    }
    return false;
  }
  if (has_external_catch(callee)) {
    if (insn) {
      log_nopt(INL_EXTERN_CATCH, callee);
    }
    return false;
  }
  std::vector<DexMethod*> make_static;
  if (cannot_inline_opcodes(caller, callee, insn, &make_static)) {
    return false;
  }
  if (!callee->rstate.force_inline()) {
    if (caller_too_large(caller->get_class(), estimated_insn_size, callee)) {
      if (insn) {
        log_nopt(INL_TOO_BIG, caller, insn);
      }
      return false;
    }

    // Don't inline code into a method that doesn't have the same (or higher)
    // required API. We don't want to bring API specific code into a class
    // where it's not supported.
    int32_t callee_api = api::LevelChecker::get_method_level(callee);
    if (callee_api != api::LevelChecker::get_min_level() &&
        callee_api > api::LevelChecker::get_method_level(caller)) {
      // check callee_api against the minimum and short-circuit because most
      // methods don't have a required api and we want that to be fast.
      if (insn) {
        log_nopt(INL_REQUIRES_API, caller, insn);
      }
      TRACE(MMINL, 4,
            "Refusing to inline %s"
            "              into %s\n because of API boundaries.",
            show_deobfuscated(callee).c_str(),
            show_deobfuscated(caller).c_str());
      return false;
    }

    if (callee->rstate.dont_inline()) {
      if (insn) {
        log_nopt(INL_DO_NOT_INLINE, caller, insn);
      }
      return false;
    }
  }

  if (make_static.size()) {
    // Only now, when we'll indicate that the method inlinable, we'll record
    // the fact that we'll have to make some methods static.
    for (auto method : make_static) {
      m_delayed_make_static.insert(method);
    }
  }

  return true;
}

/**
 * Return whether the method or any of its ancestors are in the blacklist.
 * Typically used to prevent inlining / deletion of methods that are called
 * via reflection.
 */
bool MultiMethodInliner::is_blacklisted(const DexMethod* callee) {
  auto cls = type_class(callee->get_class());
  // Enums' kept methods are all blacklisted
  if (is_enum(cls) && root(callee)) {
    return true;
  }
  while (cls != nullptr) {
    if (m_config.get_black_list().count(cls->get_type())) {
      info.blacklisted++;
      return true;
    }
    cls = type_class(cls->get_super_class());
  }
  return false;
}

bool MultiMethodInliner::is_estimate_over_max(uint64_t estimated_caller_size,
                                              const DexMethod* callee,
                                              uint64_t max) {
  // INSTRUCTION_BUFFER is added because the final method size is often larger
  // than our estimate -- during the sync phase, we may have to pick larger
  // branch opcodes to encode large jumps.
  auto callee_size = get_callee_insn_size(callee);
  if (estimated_caller_size + callee_size > max - INSTRUCTION_BUFFER) {
    info.caller_too_large++;
    return true;
  }
  return false;
}

bool MultiMethodInliner::caller_too_large(DexType* caller_type,
                                          size_t estimated_caller_size,
                                          const DexMethod* callee) {
  if (is_estimate_over_max(estimated_caller_size, callee,
                           HARD_MAX_INSTRUCTION_SIZE)) {
    return true;
  }

  if (!m_config.enforce_method_size_limit) {
    return false;
  }

  if (m_config.whitelist_no_method_limit.count(caller_type)) {
    return false;
  }

  if (is_estimate_over_max(estimated_caller_size, callee,
                           SOFT_MAX_INSTRUCTION_SIZE)) {
    return true;
  }

  return false;
}

bool MultiMethodInliner::should_inline_fast(const DexMethod* callee) {
  if (for_speed()) {
    // inline_for_speed::should_inline was used earlire to prune the static
    // call-graph
    return true;
  }

  if (callee->rstate.force_inline()) {
    return true;
  }

  const auto& callers = callee_caller.at(callee);
  auto caller_count = callers.size();
  always_assert(caller_count > 0);

  // non-root methods that are only ever called as often as there are
  // "same methods" (usually once) should always be inlined,
  // as the method can be removed afterwards
  if (caller_count <= get_same_method_implementations(callee) &&
      !root(callee)) {
    return true;
  }

  return false;
}

bool MultiMethodInliner::should_inline(const DexMethod* callee) {
  if (should_inline_fast(callee)) {
    return true;
  }

  auto res = m_should_inline.get(callee, boost::none);
  if (res) {
    return *res;
  }

  always_assert(!for_speed());
  always_assert(!callee->rstate.force_inline());
  if (too_many_callers(callee)) {
    log_nopt(INL_TOO_MANY_CALLERS, callee);
    res = false;
  } else {
    res = true;
  }
  m_should_inline.emplace(callee, res);
  return *res;
}

size_t MultiMethodInliner::get_callee_insn_size(const DexMethod* callee) {
  if (m_callee_insn_sizes) {
    auto size = m_callee_insn_sizes->get(callee, 0);
    if (size != 0) {
      return size;
    }
  }

  const IRCode* code = callee->get_code();
  auto size = code->editable_cfg_built() ? code->cfg().sum_opcode_sizes()
                                         : code->sum_opcode_sizes();
  always_assert(size > 0);
  if (m_callee_insn_sizes) {
    m_callee_insn_sizes->emplace(callee, size);
  }
  return size;
}

/*
 * Estimate additional costs if an instruction takes many source registers.
 */
static size_t get_inlined_regs_cost(size_t regs) {
  size_t cost{0};
  if (regs > 3) {
    if (regs > 5) {
      // invoke with many args will likely need extra moves
      cost += regs;
    } else {
      cost += regs / 2;
    }
  }
  return cost;
}

/*
 * Try to estimate number of code units (2 bytes each) of an instruction.
 * - Ignore internal opcodes because they do not take up any space in the
 * final dex file.
 * - Ignore move opcodes with the hope that RegAlloc will eliminate most of
 *   them.
 * - Remove return opcodes, as they will disappear when gluing things
 * together.
 */
static size_t get_inlined_cost(IRInstruction* insn) {
  auto op = insn->opcode();
  size_t cost{0};
  if (!opcode::is_internal(op) && !opcode::is_move(op) && !is_return(op)) {
    cost++;
    auto regs = insn->srcs_size() +
                ((insn->has_dest() || insn->has_move_result_pseudo()) ? 1 : 0);
    cost += get_inlined_regs_cost(regs);
    if (op == OPCODE_MOVE_EXCEPTION) {
      cost += 8; // accounting for book-keeping overhead of throw-blocks
    } else if (insn->has_method() || insn->has_field() || insn->has_type() ||
               insn->has_string() || is_conditional_branch(op)) {
      cost++;
    } else if (insn->has_data()) {
      cost += 4 + insn->get_data()->size();
    } else if (insn->has_literal()) {
      auto lit = insn->get_literal();
      if (lit < -2147483648 || lit > 2147483647) {
        cost += 4;
      } else if (lit < -32768 || lit > 32767) {
        cost += 2;
      } else if (is_const(op) && (lit < -8 || lit > 7)) {
        cost++;
      } else if (!is_const(op) && (lit < -128 || lit > 127)) {
        cost++;
      }
    }
  }
  TRACE(INLINE, 5, "  %u: %s", cost, SHOW(insn));
  return cost;
}

/*
 * Try to estimate number of code units (2 bytes each) overhead (instructions,
 * metadata) that exists for this block; this doesn't include the cost of
 * the instructions in the block, which are accounted for elsewhere.
 */
static size_t get_inlined_cost(const std::vector<cfg::Block*>& blocks,
                               size_t index) {
  auto block = blocks.at(index);
  switch (block->branchingness()) {
  case opcode::Branchingness::BRANCH_GOTO: {
    auto target = block->goes_to_only_edge();
    always_assert(target != nullptr);
    if (index == blocks.size() - 1 || blocks.at(index + 1) != target) {
      // we have a non-fallthrough goto edge
      size_t cost = 1;
      TRACE(INLINE, 5, "  %u: BRANCH_GOTO", cost);
      return cost;
    }
    break;
  }
  case opcode::Branchingness::BRANCH_SWITCH: {
    size_t cost = 4 + 3 * block->succs().size();
    TRACE(INLINE, 5, "  %u: BRANCH_SWITCH", cost);
    return cost;
  }
  default:
    break;
  }
  return 0;
}

struct InlinedCostAndDeadBlocks {
  size_t cost;
  size_t dead_blocks;
};

/*
 * Try to estimate number of code units (2 bytes each) of code. Also take
 * into account costs arising from control-flow overhead and constant
 * arguments, if any
 */
static InlinedCostAndDeadBlocks get_inlined_cost(
    const IRCode* code, const ConstantArguments* constant_arguments = nullptr) {
  auto& cfg = code->cfg();
  std::unique_ptr<constant_propagation::intraprocedural::FixpointIterator>
      intra_cp;
  if (constant_arguments) {
    intra_cp.reset(new constant_propagation::intraprocedural::FixpointIterator(
        cfg, constant_propagation::ConstantPrimitiveAnalyzer()));
    ConstantEnvironment initial_env =
        constant_propagation::interprocedural::env_with_params(
            code, *constant_arguments);
    intra_cp->run(initial_env);
  }

  size_t cost{0};
  // For each unreachable block, we'll give a small discount to
  // reflect the fact that some incoming branch instruction also will get
  // simplified or eliminated.
  size_t conditional_branch_cost_discount{0};
  size_t dead_blocks{0};
  size_t returns{0};
  if (code->editable_cfg_built()) {
    const auto blocks = code->cfg().blocks();
    for (size_t i = 0; i < blocks.size(); ++i) {
      auto block = blocks.at(i);
      if (intra_cp && intra_cp->get_entry_state_at(block).is_bottom()) {
        // we found an unreachable block
        for (const auto edge : block->preds()) {
          const auto& pred_env = intra_cp->get_entry_state_at(edge->src());
          if (!pred_env.is_bottom() && edge->type() == cfg::EDGE_BRANCH) {
            // A conditional branch instruction is going to disppear
            conditional_branch_cost_discount += 1;
          }
        }
        dead_blocks++;
        continue;
      }
      cost += get_inlined_cost(blocks, i);
      for (auto& mie : InstructionIterable(block)) {
        auto insn = mie.insn;
        cost += get_inlined_cost(insn);
        if (is_return(insn->opcode())) {
          returns++;
        }
      }
    }
  } else {
    editable_cfg_adapter::iterate(code, [&](const MethodItemEntry& mie) {
      auto insn = mie.insn;
      cost += get_inlined_cost(insn);
      if (is_return(insn->opcode())) {
        returns++;
      }
      return editable_cfg_adapter::LOOP_CONTINUE;
    });
  }
  if (returns > 1) {
    // if there's more than one return, gotos will get introduced to merge
    // control flow
    cost += returns - 1;
  }

  return {cost - std::min(cost, conditional_branch_cost_discount), dead_blocks};
}

size_t MultiMethodInliner::get_inlined_cost(const DexMethod* callee) {
  auto opt_inlined_cost = m_inlined_costs.get(callee, boost::none);
  if (opt_inlined_cost) {
    return *opt_inlined_cost;
  }

  std::atomic<size_t> callees_analyzed{0};
  std::atomic<size_t> callees_unreachable_blocks{0};
  std::atomic<size_t> inlined_cost{::get_inlined_cost(callee->get_code()).cost};
  auto callee_constant_arguments_it = m_callee_constant_arguments.find(callee);
  if (callee_constant_arguments_it != m_callee_constant_arguments.end() &&
      inlined_cost <= MAX_COST_FOR_CONSTANT_PROPAGATION) {
    const auto& callee_constant_arguments =
        callee_constant_arguments_it->second;
    auto process_key = [&](const ConstantArgumentsOccurrences& cao) {
      const auto& constant_arguments = cao.first;
      const auto count = cao.second;
      TRACE(INLINE, 5, "[too_many_callers] get_inlined_cost %s", SHOW(callee));
      auto res = ::get_inlined_cost(callee->get_code(), &constant_arguments);
      TRACE(INLINE, 4,
            "[too_many_callers] get_inlined_cost with %zu constant invoke "
            "params %s @ %s: cost %zu (dead blocks: %zu)",
            constant_arguments.is_top() ? 0 : constant_arguments.size(),
            get_key(constant_arguments).c_str(), SHOW(callee), res.cost,
            res.dead_blocks);
      callees_unreachable_blocks += res.dead_blocks * count;
      inlined_cost += res.cost * count;
      callees_analyzed += count;
    };

    if (callee_constant_arguments.size() > 1 &&
        callee_constant_arguments.size() * inlined_cost >=
            MIN_COST_FOR_PARALLELIZATION) {
      // TODO: This parallelization happens independently (in addition to) the
      // parallelization via m_async_method_executor. This should be combined,
      // using a single thread pool.
      inlined_cost = 0;
      auto num_threads =
          std::min(redex_parallel::default_num_threads(),
                   (unsigned int)callee_constant_arguments.size());
      auto wq = workqueue_foreach<ConstantArgumentsOccurrences>(process_key,
                                                                num_threads);
      for (auto& p : callee_constant_arguments) {
        wq.add_item(p);
      }
      wq.run_all();
    } else {
      inlined_cost = 0;
      for (auto& p : callee_constant_arguments) {
        process_key(p);
      }
    }

    always_assert(callees_analyzed > 0);
    inlined_cost = inlined_cost / callees_analyzed;
  }
  TRACE(INLINE, 4, "[too_many_callers] get_inlined_cost %s: %u", SHOW(callee),
        (size_t)inlined_cost);
  m_inlined_costs.update(
      callee,
      [&](const DexMethod*, boost::optional<size_t>& value, bool exists) {
        if (exists) {
          // We wasted some work, and some other thread beat us. Oh well...
          always_assert(*value == inlined_cost);
          return;
        }
        value = inlined_cost;
        if (callees_analyzed == 0) {
          return;
        }
        info.constant_invoke_callees_analyzed += callees_analyzed;
        info.constant_invoke_callees_unreachable_blocks +=
            callees_unreachable_blocks;
      });
  return inlined_cost;
}

size_t MultiMethodInliner::get_same_method_implementations(
    const DexMethod* callee) {
  auto it = m_same_method_implementations.find(callee);
  if (it != m_same_method_implementations.end()) {
    return it->second;
  }
  return 1;
}

bool MultiMethodInliner::can_inline_init(const DexMethod* init_method) {
  auto opt_can_inline_init = m_can_inline_init.get(init_method, boost::none);
  if (opt_can_inline_init) {
    return *opt_can_inline_init;
  }

  bool res = constructor_analysis::can_inline_init(init_method);
  m_can_inline_init.update(
      init_method,
      [&](const DexMethod*, boost::optional<bool>& value, bool exists) {
        if (exists) {
          // We wasted some work, and some other thread beat us. Oh well...
          always_assert(*value == res);
          return;
        }
        value = res;
      });
  return res;
}

bool MultiMethodInliner::too_many_callers(const DexMethod* callee) {
  const auto& callers = callee_caller.at(callee);
  auto caller_count = callers.size();
  always_assert(caller_count > 0);
  auto same_method_implementations = get_same_method_implementations(callee);
  always_assert(caller_count > same_method_implementations || root(callee));

  // 1. Determine costs of inlining

  size_t inlined_cost = get_inlined_cost(callee);

  if (m_mode != IntraDex) {
    bool have_all_callers_same_class;
    auto opt_have_all_callers_same_class =
        m_callers_in_same_class.get(callee, boost::none);
    if (opt_have_all_callers_same_class) {
      have_all_callers_same_class = *opt_have_all_callers_same_class;
    } else {
      auto callee_class = callee->get_class();
      have_all_callers_same_class = true;
      for (auto caller : callers) {
        if (caller->get_class() != callee_class) {
          have_all_callers_same_class = false;
          break;
        }
      }
      m_callers_in_same_class.emplace(callee, have_all_callers_same_class);
    }

    if (!have_all_callers_same_class) {
      // Inlining methods into different classes might lead to worse
      // cross-dex-ref minimization results.
      inlined_cost += COST_INTER_DEX_SOME_CALLERS_DIFFERENT_CLASSES;
    }
  }

  // 2. Determine costs of keeping the invoke instruction

  size_t invoke_cost = callee->get_proto()->is_void()
                           ? COST_INVOKE_WITHOUT_RESULT
                           : COST_INVOKE_WITH_RESULT;
  invoke_cost += get_inlined_regs_cost(callee->get_proto()->get_args()->size());
  TRACE(INLINE, 3,
        "[too_many_callers] %u calls to %s; cost: inlined %u, invoke %u",
        caller_count, SHOW(callee), inlined_cost, invoke_cost);

  // 3. Assess whether we should not inline

  if (root(callee)) {
    if (m_config.inline_small_non_deletables) {
      // Let's just consider this particular inlining opportunity
      return inlined_cost > invoke_cost;
    } else {
      return true;
    }
  }

  // Let's just consider this particular inlining opportunity
  if (inlined_cost <= invoke_cost) {
    return false;
  }

  // Can we inline the init-callee into all callers?
  // If not, then we can give up, as there's no point in making the case that
  // we can eliminate the callee method based on pervasive inlining.
  if (m_analyze_and_prune_inits && method::is_init(callee)) {
    if (!callee->get_code()->editable_cfg_built()) {
      return true;
    }
    if (!can_inline_init(callee)) {
      std::unordered_set<DexMethod*> callers_set(callers.begin(),
                                                 callers.end());
      for (auto caller : callers_set) {
        if (!method::is_init(caller) ||
            caller->get_class() != callee->get_class() ||
            !caller->get_code()->editable_cfg_built() ||
            !constructor_analysis::can_inline_inits_in_same_class(
                caller, callee,
                /* callsite_insn */ nullptr)) {
          return true;
        }
      }
    }
  }

  if (m_config.multiple_callers) {
    // The cost of keeping a method amounts of somewhat fixed metadata overhead,
    // plus the method body, which we approximate with the inlined cost.
    size_t method_cost = COST_METHOD + get_inlined_cost(callee);
    auto methods_cost = method_cost * same_method_implementations;

    // If we inline invocations to this method everywhere, we could delete the
    // method. Is this worth it, given the number of callsites and costs
    // involved?
    return inlined_cost * caller_count >
           invoke_cost * caller_count + methods_cost;
  }

  return true;
}

bool MultiMethodInliner::caller_is_blacklisted(const DexMethod* caller) {
  auto cls = caller->get_class();
  if (m_config.get_caller_black_list().count(cls)) {
    info.blacklisted++;
    return true;
  }
  return false;
}

/**
 * Returns true if the callee has catch type which is external and not public,
 * in which case we cannot inline.
 */
bool MultiMethodInliner::has_external_catch(const DexMethod* callee) {
  const IRCode* code = callee->get_code();
  std::vector<DexType*> types;
  if (code->editable_cfg_built()) {
    code->cfg().gather_catch_types(types);
  } else {
    code->gather_catch_types(types);
  }
  for (auto type : types) {
    auto cls = type_class(type);
    if (cls != nullptr && cls->is_external() && !is_public(cls)) {
      return true;
    }
  }
  return false;
}

/**
 * Analyze opcodes in the callee to see if they are problematic for inlining.
 */
bool MultiMethodInliner::cannot_inline_opcodes(
    const DexMethod* caller,
    const DexMethod* callee,
    const IRInstruction* invk_insn,
    std::vector<DexMethod*>* make_static) {
  int ret_count = 0;
  bool can_inline = true;
  editable_cfg_adapter::iterate(
      callee->get_code(), [&](const MethodItemEntry& mie) {
        auto insn = mie.insn;
        if (create_vmethod(insn, callee, caller, make_static)) {
          if (invk_insn) {
            log_nopt(INL_CREATE_VMETH, caller, invk_insn);
          }
          can_inline = false;
          return editable_cfg_adapter::LOOP_BREAK;
        }
        // if the caller and callee are in the same class, we don't have to
        // worry about invoke supers, or unknown virtuals -- private /
        // protected methods will remain accessible
        if (caller->get_class() != callee->get_class()) {
          if (nonrelocatable_invoke_super(insn)) {
            if (invk_insn) {
              log_nopt(INL_HAS_INVOKE_SUPER, caller, invk_insn);
            }
            can_inline = false;
            return editable_cfg_adapter::LOOP_BREAK;
          }
          if (unknown_virtual(insn)) {
            if (invk_insn) {
              log_nopt(INL_UNKNOWN_VIRTUAL, caller, invk_insn);
            }
            can_inline = false;
            return editable_cfg_adapter::LOOP_BREAK;
          }
          if (unknown_field(insn)) {
            if (invk_insn) {
              log_nopt(INL_UNKNOWN_FIELD, caller, invk_insn);
            }
            can_inline = false;
            return editable_cfg_adapter::LOOP_BREAK;
          }
          if (check_android_os_version(insn)) {
            can_inline = false;
            return editable_cfg_adapter::LOOP_BREAK;
          }
        }
        if (!m_config.throws_inline && insn->opcode() == OPCODE_THROW) {
          info.throws++;
          can_inline = false;
          return editable_cfg_adapter::LOOP_BREAK;
        }
        if (is_return(insn->opcode())) {
          ++ret_count;
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
  // The IRCode inliner can't handle callees with more than one return
  // statement (normally one, the way dx generates code). That allows us to
  // make a simple inline strategy where we don't have to worry about creating
  // branches from the multiple returns to the main code
  //
  // d8 however, generates code with multiple return statements in general.
  // The CFG inliner can handle multiple return callees.
  if (ret_count > 1 && !m_config.use_cfg_inliner) {
    info.multi_ret++;
    if (invk_insn) {
      log_nopt(INL_MULTIPLE_RETURNS, callee);
    }
    can_inline = false;
  }
  return !can_inline;
}

/**
 * Check if a visibility/accessibility change would turn a method referenced
 * in a callee to virtual methods as they are inlined into the caller.
 * That is, once a callee is inlined we need to ensure that everything that
 * was referenced by a callee is visible and accessible in the caller context.
 * This step would not be needed if we changed all private instance to static.
 */
bool MultiMethodInliner::create_vmethod(IRInstruction* insn,
                                        const DexMethod* callee,
                                        const DexMethod* caller,
                                        std::vector<DexMethod*>* make_static) {
  auto opcode = insn->opcode();
  if (opcode == OPCODE_INVOKE_DIRECT) {
    auto method = resolver(insn->get_method(), MethodSearch::Direct);
    if (method == nullptr) {
      info.need_vmethod++;
      return true;
    }
    always_assert(method->is_def());
    if (caller->get_class() == callee->get_class()) {
      // No need to give up here, or make it static. Visibility is just fine.
      return false;
    }
    if (method::is_init(method)) {
      if (!method->is_concrete() && !is_public(method)) {
        info.non_pub_ctor++;
        return true;
      }
      // concrete ctors we can handle because they stay invoke_direct
      return false;
    }
    if (can_rename(method)) {
      make_static->push_back(method);
    } else {
      info.need_vmethod++;
      return true;
    }
  }
  return false;
}

/**
 * Return true if a callee contains an invoke super to a different method
 * in the hierarchy.
 * Inlining an invoke_super off its class hierarchy would break the verifier.
 */
bool MultiMethodInliner::nonrelocatable_invoke_super(IRInstruction* insn) {
  if (insn->opcode() == OPCODE_INVOKE_SUPER) {
    info.invoke_super++;
    return true;
  }
  return false;
}

/**
 * The callee contains an invoke to a virtual method we either do not know
 * or it's not public. Given the caller may not be in the same
 * hierarchy/package we cannot inline it unless we make the method public.
 * But we need to make all methods public across the hierarchy and for methods
 * we don't know we have no idea whether the method was public or not anyway.
 */
bool MultiMethodInliner::unknown_virtual(IRInstruction* insn) {
  if (insn->opcode() == OPCODE_INVOKE_VIRTUAL) {
    auto method = insn->get_method();
    auto res_method = resolver(method, MethodSearch::Virtual);
    if (res_method == nullptr) {
      info.unresolved_methods++;
      if (unknown_virtuals::is_method_known_to_be_public(method)) {
        info.known_public_methods++;
        return false;
      }

      info.escaped_virtual++;
      return true;
    }
    if (res_method->is_external() && !is_public(res_method)) {
      info.non_pub_virtual++;
      return true;
    }
  }
  return false;
}

/**
 * The callee contains a *get/put instruction to an unknown field.
 * Given the caller may not be in the same hierarchy/package we cannot inline
 * it unless we make the field public.
 * But we need to make all fields public across the hierarchy and for fields
 * we don't know we have no idea whether the field was public or not anyway.
 */
bool MultiMethodInliner::unknown_field(IRInstruction* insn) {
  if (is_ifield_op(insn->opcode()) || is_sfield_op(insn->opcode())) {
    auto ref = insn->get_field();
    DexField* field = resolve_field(ref, is_sfield_op(insn->opcode())
                                             ? FieldSearch::Static
                                             : FieldSearch::Instance);
    if (field == nullptr) {
      info.escaped_field++;
      return true;
    }
    if (!field->is_concrete() && !is_public(field)) {
      info.non_pub_field++;
      return true;
    }
  }
  return false;
}

/**
 * return true if `insn` is
 *   sget android.os.Build.VERSION.SDK_INT
 */
bool MultiMethodInliner::check_android_os_version(IRInstruction* insn) {
  // Referencing a method or field that doesn't exist on the OS version of the
  // current device causes a "soft error" for the entire class that the
  // reference resides in. Soft errors aren't worrisome from a correctness
  // perspective (though they may cause the class to run slower on some
  // devices) but there's a bug in Android 5 that triggers an erroneous "hard
  // error" after a "soft error".
  //
  // The exact conditions that trigger the Android 5 bug aren't currently
  // known. As a quick fix, we're refusing to inline methods that check the
  // OS's version. This generally works because the reference to the
  // non-existent field/method is usually guarded by checking that
  // `android.os.build.VERSION.SDK_INT` is larger than the required api level.
  auto op = insn->opcode();
  if (is_sget(op)) {
    auto ref = insn->get_field();
    DexField* field = resolve_field(ref, FieldSearch::Static);
    if (field != nullptr &&
        field == DexField::get_field("Landroid/os/Build$VERSION;.SDK_INT:I")) {
      return true;
    }
  }
  return false;
}

bool MultiMethodInliner::cross_store_reference(const DexMethod* caller,
                                               const DexMethod* callee) {
  size_t store_idx = xstores.get_store_idx(caller->get_class());
  bool has_cross_store_ref = false;
  editable_cfg_adapter::iterate(
      callee->get_code(), [&](const MethodItemEntry& mie) {
        auto insn = mie.insn;
        if (insn->has_type()) {
          if (xstores.illegal_ref(store_idx, insn->get_type())) {
            info.cross_store++;
            has_cross_store_ref = true;
            return editable_cfg_adapter::LOOP_BREAK;
          }
        } else if (insn->has_method()) {
          auto meth = insn->get_method();
          if (xstores.illegal_ref(store_idx, meth->get_class())) {
            info.cross_store++;
            has_cross_store_ref = true;
            return editable_cfg_adapter::LOOP_BREAK;
          }
          auto proto = meth->get_proto();
          if (xstores.illegal_ref(store_idx, proto->get_rtype())) {
            info.cross_store++;
            has_cross_store_ref = true;
            return editable_cfg_adapter::LOOP_BREAK;
          }
          auto args = proto->get_args();
          if (args == nullptr) {
            return editable_cfg_adapter::LOOP_CONTINUE;
          }
          for (const auto& arg : args->get_type_list()) {
            if (xstores.illegal_ref(store_idx, arg)) {
              info.cross_store++;
              has_cross_store_ref = true;
              return editable_cfg_adapter::LOOP_BREAK;
            }
          }
        } else if (insn->has_field()) {
          auto field = insn->get_field();
          if (xstores.illegal_ref(store_idx, field->get_class()) ||
              xstores.illegal_ref(store_idx, field->get_type())) {
            info.cross_store++;
            has_cross_store_ref = true;
            return editable_cfg_adapter::LOOP_BREAK;
          }
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
  return has_cross_store_ref;
}

void MultiMethodInliner::delayed_change_visibilities() {
  walk::parallel::code(m_scope, [&](DexMethod* method, IRCode& code) {
    auto it = m_delayed_change_visibilities->find(method);
    if (it == m_delayed_change_visibilities->end()) {
      return;
    }
    auto& scopes = it->second;
    for (auto scope : scopes) {
      TRACE(MMINL, 6, "checking visibility usage of members in %s",
            SHOW(method));
      change_visibility(method, scope);
    }
  });
}

void MultiMethodInliner::delayed_invoke_direct_to_static() {
  // We sort the methods here because make_static renames methods on
  // collision, and which collisions occur is order-dependent. E.g. if we have
  // the following methods in m_delayed_make_static:
  //
  //   Foo Foo::bar()
  //   Foo Foo::bar(Foo f)
  //
  // making Foo::bar() static first would make it collide with Foo::bar(Foo
  // f), causing it to get renamed to bar$redex0(). But if Foo::bar(Foo f)
  // gets static-ified first, it becomes Foo::bar(Foo f, Foo f), so when bar()
  // gets made static later there is no collision. So in the interest of
  // having reproducible binaries, we sort the methods first.
  //
  // Also, we didn't use an std::set keyed by method signature here because
  // make_static is mutating the signatures. The tree that implements the set
  // would have to be rebalanced after the mutations.
  std::vector<DexMethod*> methods(m_delayed_make_static.begin(),
                                  m_delayed_make_static.end());
  std::sort(methods.begin(), methods.end(), compare_dexmethods);
  for (auto method : methods) {
    TRACE(MMINL, 6, "making %s static", method->get_name()->c_str());
    mutators::make_static(method);
  }
  walk::parallel::opcodes(m_scope, [](DexMethod* meth) { return true; },
                          [&](DexMethod*, IRInstruction* insn) {
                            auto op = insn->opcode();
                            if (op == OPCODE_INVOKE_DIRECT) {
                              auto m = insn->get_method()->as_def();
                              if (m && m_delayed_make_static.count_unsafe(m)) {
                                insn->set_opcode(OPCODE_INVOKE_STATIC);
                              }
                            }
                          });
}

namespace {

using RegMap = transform::RegMap;

/*
 * Expands the caller register file by the size of the callee register file,
 * and allocates the high registers to the callee. E.g. if we have a caller
 * with registers_size of M and a callee with registers_size N, this function
 * will resize the caller's register file to M + N and map register k in the
 * callee to M + k in the caller. It also inserts move instructions to map the
 * callee arguments to the newly allocated registers.
 */
std::unique_ptr<RegMap> gen_callee_reg_map(IRCode* caller_code,
                                           const IRCode* callee_code,
                                           const IRList::iterator& invoke_it) {
  auto callee_reg_start = caller_code->get_registers_size();
  auto insn = invoke_it->insn;
  auto reg_map = std::make_unique<RegMap>();

  // generate the callee register map
  for (reg_t i = 0; i < callee_code->get_registers_size(); ++i) {
    reg_map->emplace(i, callee_reg_start + i);
  }

  // generate and insert the move instructions
  auto param_insns = InstructionIterable(callee_code->get_param_instructions());
  auto param_it = param_insns.begin();
  auto param_end = param_insns.end();
  for (size_t i = 0; i < insn->srcs_size(); ++i, ++param_it) {
    always_assert(param_it != param_end);
    auto mov = (new IRInstruction(
                    opcode::load_param_to_move(param_it->insn->opcode())))
                   ->set_src(0, insn->src(i))
                   ->set_dest(callee_reg_start + param_it->insn->dest());
    caller_code->insert_before(invoke_it, mov);
  }
  caller_code->set_registers_size(callee_reg_start +
                                  callee_code->get_registers_size());
  return reg_map;
}

/**
 * Create a move instruction given a return instruction in a callee and
 * a move-result instruction in a caller.
 */
IRInstruction* move_result(IRInstruction* res, IRInstruction* move_res) {
  auto move_opcode = opcode::return_to_move(res->opcode());
  IRInstruction* move = (new IRInstruction(move_opcode))
                            ->set_dest(move_res->dest())
                            ->set_src(0, res->src(0));
  return move;
}

/*
 * Map the callee's param registers to the argument registers of the caller.
 * Any other callee register N will get mapped to caller_registers_size + N.
 * The resulting callee code can then be appended to the caller's code without
 * any register conflicts.
 */
void remap_callee_for_tail_call(const IRCode* caller_code,
                                IRCode* callee_code,
                                const IRList::iterator& invoke_it) {
  RegMap reg_map;
  auto insn = invoke_it->insn;
  auto callee_reg_start = caller_code->get_registers_size();

  auto param_insns = InstructionIterable(callee_code->get_param_instructions());
  auto param_it = param_insns.begin();
  auto param_end = param_insns.end();
  for (size_t i = 0; i < insn->srcs_size(); ++i, ++param_it) {
    always_assert_log(param_it != param_end, "no param insns\n%s",
                      SHOW(callee_code));
    reg_map[param_it->insn->dest()] = insn->src(i);
  }
  for (size_t i = 0; i < callee_code->get_registers_size(); ++i) {
    if (reg_map.count(i) != 0) {
      continue;
    }
    reg_map[i] = callee_reg_start + i;
  }
  transform::remap_registers(callee_code, reg_map);
}

void cleanup_callee_debug(IRCode* callee_code) {
  std::unordered_set<reg_t> valid_regs;
  auto it = callee_code->begin();
  while (it != callee_code->end()) {
    auto& mei = *it++;
    if (mei.type == MFLOW_DEBUG) {
      switch (mei.dbgop->opcode()) {
      case DBG_SET_PROLOGUE_END:
        callee_code->erase(callee_code->iterator_to(mei));
        break;
      case DBG_START_LOCAL:
      case DBG_START_LOCAL_EXTENDED: {
        auto reg = mei.dbgop->uvalue();
        valid_regs.insert(reg);
        break;
      }
      case DBG_END_LOCAL:
      case DBG_RESTART_LOCAL: {
        auto reg = mei.dbgop->uvalue();
        if (valid_regs.find(reg) == valid_regs.end()) {
          callee_code->erase(callee_code->iterator_to(mei));
        }
        break;
      }
      default:
        break;
      }
    }
  }
}
} // namespace

/*
 * For splicing a callee's IRList into a caller.
 */
class MethodSplicer {
  IRCode* m_mtcaller;
  IRCode* m_mtcallee;
  MethodItemEntryCloner m_mie_cloner;
  const RegMap& m_callee_reg_map;
  DexPosition* m_invoke_position;
  MethodItemEntry* m_active_catch;
  std::unordered_set<reg_t> m_valid_dbg_regs;

 public:
  MethodSplicer(IRCode* mtcaller,
                IRCode* mtcallee,
                const RegMap& callee_reg_map,
                DexPosition* invoke_position,
                MethodItemEntry* active_catch)
      : m_mtcaller(mtcaller),
        m_mtcallee(mtcallee),
        m_callee_reg_map(callee_reg_map),
        m_invoke_position(invoke_position),
        m_active_catch(active_catch) {}

  MethodItemEntry* clone(MethodItemEntry* mie) {
    auto result = m_mie_cloner.clone(mie);
    return result;
  }

  void operator()(const IRList::iterator& insert_pos,
                  const IRList::iterator& fcallee_start,
                  const IRList::iterator& fcallee_end) {
    std::vector<DexPosition*> positions_to_fix;
    for (auto it = fcallee_start; it != fcallee_end; ++it) {
      if (should_skip_debug(&*it)) {
        continue;
      }
      if (it->type == MFLOW_OPCODE &&
          opcode::is_load_param(it->insn->opcode())) {
        continue;
      }
      auto mie = clone(&*it);
      transform::remap_registers(*mie, m_callee_reg_map);
      if (mie->type == MFLOW_TRY && m_active_catch != nullptr) {
        auto tentry = mie->tentry;
        // try ranges cannot be nested, so we flatten them here
        switch (tentry->type) {
        case TRY_START:
          m_mtcaller->insert_before(
              insert_pos, *(new MethodItemEntry(TRY_END, m_active_catch)));
          m_mtcaller->insert_before(insert_pos, *mie);
          break;
        case TRY_END:
          m_mtcaller->insert_before(insert_pos, *mie);
          m_mtcaller->insert_before(
              insert_pos, *(new MethodItemEntry(TRY_START, m_active_catch)));
          break;
        }
      } else {
        if (mie->type == MFLOW_POSITION && mie->pos->parent == nullptr) {
          mie->pos->parent = m_invoke_position;
        }
        // if a handler list does not terminate in a catch-all, have it point
        // to the parent's active catch handler. TODO: Make this more precise
        // by checking if the parent catch type is a subtype of the callee's.
        if (mie->type == MFLOW_CATCH && mie->centry->next == nullptr &&
            mie->centry->catch_type != nullptr) {
          mie->centry->next = m_active_catch;
        }
        m_mtcaller->insert_before(insert_pos, *mie);
      }
    }
  }

  void fix_parent_positions() {
    m_mie_cloner.fix_parent_positions(m_invoke_position);
  }

 private:
  /* We need to skip two cases:
   * Duplicate DBG_SET_PROLOGUE_END
   * Uninitialized parameters
   *
   * The parameter names are part of the debug info for the method.
   * The technically correct solution would be to make a start
   * local for each of them.  However, that would also imply another
   * end local after the tail to correctly set what the register
   * is at the end.  This would bloat the debug info parameters for
   * a corner case.
   *
   * Instead, we just delete locals lifetime information for parameters.
   * This is an exceedingly rare case triggered by goofy code that
   * reuses parameters as locals.
   */
  bool should_skip_debug(const MethodItemEntry* mei) {
    if (mei->type != MFLOW_DEBUG) {
      return false;
    }
    switch (mei->dbgop->opcode()) {
    case DBG_SET_PROLOGUE_END:
      return true;
    case DBG_START_LOCAL:
    case DBG_START_LOCAL_EXTENDED: {
      auto reg = mei->dbgop->uvalue();
      m_valid_dbg_regs.insert(reg);
      return false;
    }
    case DBG_END_LOCAL:
    case DBG_RESTART_LOCAL: {
      auto reg = mei->dbgop->uvalue();
      if (m_valid_dbg_regs.find(reg) == m_valid_dbg_regs.end()) {
        return true;
      }
    }
    default:
      return false;
    }
  }
};

namespace inliner {

DexPosition* last_position_before(const IRList::const_iterator& it,
                                  const IRCode* code) {
  // we need to decrement the reverse iterator because it gets constructed
  // as pointing to the element preceding pos
  auto position_it = std::prev(IRList::const_reverse_iterator(it));
  const auto& rend = code->rend();
  while (++position_it != rend && position_it->type != MFLOW_POSITION)
    ;
  return position_it == rend ? nullptr : position_it->pos.get();
}

void inline_method(DexMethod* caller,
                   IRCode* callee_code,
                   const IRList::iterator& pos) {
  change_visibility(callee_code, caller->get_class());
  inline_method_unsafe(caller->get_code(), callee_code, pos);
}

void inline_method_unsafe(IRCode* caller_code,
                          IRCode* callee_code,
                          const IRList::iterator& pos) {
  TRACE(INL, 5, "caller code:\n%s", SHOW(caller_code));
  TRACE(INL, 5, "callee code:\n%s", SHOW(callee_code));

  auto callee_reg_map = gen_callee_reg_map(caller_code, callee_code, pos);

  // find the move-result after the invoke, if any. Must be the first
  // instruction after the invoke
  auto move_res = pos;
  while (move_res++ != caller_code->end() && move_res->type != MFLOW_OPCODE)
    ;
  if (!opcode::is_move_result(move_res->insn->opcode())) {
    move_res = caller_code->end();
  }

  // find the last position entry before the invoke.
  const auto invoke_position = last_position_before(pos, caller_code);
  if (invoke_position) {
    TRACE(INL, 3, "Inlining call at %s:%d", invoke_position->file->c_str(),
          invoke_position->line);
  }

  // check if we are in a try block
  auto caller_catch = transform::find_active_catch(caller_code, pos);

  const auto& ret_it = std::find_if(
      callee_code->begin(), callee_code->end(), [](const MethodItemEntry& mei) {
        return mei.type == MFLOW_OPCODE && is_return(mei.insn->opcode());
      });

  auto splice = MethodSplicer(caller_code, callee_code, *callee_reg_map,
                              invoke_position, caller_catch);
  // Copy the callee up to the return. Everything else we push at the end
  // of the caller
  splice(pos, callee_code->begin(), ret_it);

  // try items can span across a return opcode
  auto callee_catch =
      splice.clone(transform::find_active_catch(callee_code, ret_it));
  if (callee_catch != nullptr) {
    caller_code->insert_before(pos,
                               *(new MethodItemEntry(TRY_END, callee_catch)));
    if (caller_catch != nullptr) {
      caller_code->insert_before(
          pos, *(new MethodItemEntry(TRY_START, caller_catch)));
    }
  }

  if (move_res != caller_code->end() && ret_it != callee_code->end()) {
    auto ret_insn = std::make_unique<IRInstruction>(*ret_it->insn);
    transform::remap_registers(ret_insn.get(), *callee_reg_map);
    IRInstruction* move = move_result(ret_insn.get(), move_res->insn);
    auto move_mei = new MethodItemEntry(move);
    caller_code->insert_before(pos, *move_mei);
  }
  // ensure that the caller's code after the inlined method retain their
  // original position
  if (invoke_position) {
    caller_code->insert_before(
        pos,
        *(new MethodItemEntry(
            std::make_unique<DexPosition>(*invoke_position))));
  }

  // remove invoke
  caller_code->erase_and_dispose(pos);
  // remove move_result
  if (move_res != caller_code->end()) {
    caller_code->erase_and_dispose(move_res);
  }

  if (ret_it != callee_code->end()) {
    if (callee_catch != nullptr) {
      caller_code->push_back(*(new MethodItemEntry(TRY_START, callee_catch)));
    } else if (caller_catch != nullptr) {
      caller_code->push_back(*(new MethodItemEntry(TRY_START, caller_catch)));
    }

    if (std::next(ret_it) != callee_code->end()) {
      const auto return_position = last_position_before(ret_it, callee_code);
      if (return_position) {
        // If there are any opcodes between the callee's return and its next
        // position, we need to re-mark them with the correct line number,
        // otherwise they would inherit the line number from the end of the
        // caller.
        auto new_pos = std::make_unique<DexPosition>(*return_position);
        // We want its parent to be the same parent as other inlined code.
        new_pos->parent = invoke_position;
        caller_code->push_back(*(new MethodItemEntry(std::move(new_pos))));
      }
    }

    // Copy the opcodes in the callee after the return and put them at the end
    // of the caller.
    splice(caller_code->end(), std::next(ret_it), callee_code->end());
    if (caller_catch != nullptr) {
      caller_code->push_back(*(new MethodItemEntry(TRY_END, caller_catch)));
    }
  }
  splice.fix_parent_positions();
  TRACE(INL, 5, "post-inline caller code:\n%s", SHOW(caller_code));
}

void inline_tail_call(DexMethod* caller,
                      DexMethod* callee,
                      IRList::iterator pos) {
  TRACE(INL, 2, "caller: %s\ncallee: %s", SHOW(caller), SHOW(callee));
  auto* caller_code = caller->get_code();
  auto* callee_code = callee->get_code();

  remap_callee_for_tail_call(caller_code, callee_code, pos);
  caller_code->set_registers_size(caller_code->get_registers_size() +
                                  callee_code->get_registers_size());

  cleanup_callee_debug(callee_code);
  auto it = callee_code->begin();
  while (it != callee_code->end()) {
    auto& mei = *it++;
    if (mei.type == MFLOW_OPCODE && opcode::is_load_param(mei.insn->opcode())) {
      continue;
    }
    callee_code->erase(callee_code->iterator_to(mei));
    caller_code->insert_before(pos, mei);
  }
  // Delete the vestigial tail.
  while (pos != caller_code->end()) {
    if (pos->type == MFLOW_OPCODE) {
      pos = caller_code->erase_and_dispose(pos);
    } else {
      ++pos;
    }
  }
}

// return true on successful inlining, false otherwise
bool inline_with_cfg(DexMethod* caller_method,
                     DexMethod* callee_method,
                     IRInstruction* callsite) {

  auto caller_code = caller_method->get_code();
  always_assert(caller_code->editable_cfg_built());
  auto& caller_cfg = caller_code->cfg();
  const cfg::InstructionIterator& callsite_it = caller_cfg.find_insn(callsite);
  if (callsite_it.is_end()) {
    // The callsite is not in the caller cfg. This is probably because the
    // callsite pointer is stale. Maybe the callsite's block was deleted since
    // the time the callsite was found.
    //
    // This could have happened if a previous inlining caused a block to be
    // unreachable, and that block was deleted when the CFG was simplified.
    return false;
  }

  // Logging before the call to inline_cfg to get the most relevant line
  // number near callsite before callsite gets replaced. Should be ok as
  // inline_cfg does not fail to inline.
  log_opt(INLINED, caller_method, callsite);

  auto callee_code = callee_method->get_code();
  always_assert(callee_code->editable_cfg_built());
  cfg::CFGInliner::inline_cfg(&caller_cfg, callsite_it, callee_code->cfg());

  return true;
}

} // namespace inliner
