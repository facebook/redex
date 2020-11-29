/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Inliner.h"

#include <cstdint>
#include <utility>

#include "ABExperimentContext.h"
#include "ApiLevelChecker.h"
#include "CFGInliner.h"
#include "ConcurrentContainers.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationWholeProgramState.h"
#include "ConstructorAnalysis.h"
#include "ControlFlow.h"
#include "DexInstruction.h"
#include "DexPosition.h"
#include "DexUtil.h"
#include "EditableCfgAdapter.h"
#include "IRInstruction.h"
#include "InlineForSpeed.h"
#include "InlinerConfig.h"
#include "LocalDce.h"
#include "Macros.h"
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
constexpr uint64_t HARD_MAX_INSTRUCTION_SIZE = UINT64_C(1) << 32;

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
    const method_profiles::MethodProfiles* method_profiles,
    const std::unordered_map<const DexMethod*, size_t>*
        same_method_implementations,
    bool analyze_and_prune_inits,
    const std::unordered_set<DexMethodRef*>& configured_pure_methods)
    : resolver(std::move(resolve_fn)),
      xstores(stores),
      m_scope(scope),
      m_config(config),
      m_mode(mode),
      m_inline_for_speed(method_profiles),
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
                    if (opcode::is_an_invoke(insn->opcode())) {
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
          if (opcode::is_an_invoke(insn->opcode())) {
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

  if (m_config.run_cse || m_config.run_local_dce) {
    m_pure_methods.insert(configured_pure_methods.begin(),
                          configured_pure_methods.end());
    auto immutable_getters = get_immutable_getters(scope);
    m_pure_methods.insert(immutable_getters.begin(), immutable_getters.end());
    if (m_config.run_cse) {
      m_cse_shared_state =
          std::make_unique<cse_impl::SharedState>(m_pure_methods);
    }
    if (m_config.run_local_dce) {
      std::unique_ptr<const method_override_graph::Graph> owned_override_graph;
      const method_override_graph::Graph* override_graph;
      if (m_config.run_cse) {
        override_graph = m_cse_shared_state->get_method_override_graph();
      } else {
        owned_override_graph = method_override_graph::build_graph(scope);
        override_graph = owned_override_graph.get();
      }
      std::unordered_set<const DexMethod*> computed_no_side_effects_methods;
      auto computed_no_side_effects_methods_iterations =
          compute_no_side_effects_methods(scope, override_graph, m_pure_methods,
                                          &computed_no_side_effects_methods);
      for (auto m : computed_no_side_effects_methods) {
        m_pure_methods.insert(const_cast<DexMethod*>(m));
      }
    }
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
  // We'll normalize the order by putting things into a std::map
  std::map<reg_t, SignedConstantDomain> bindings;
  for (auto& p : constant_arguments.bindings()) {
    always_assert(!p.second.is_top());
    if (const auto& signed_value = p.second.maybe_get<SignedConstantDomain>()) {
      bindings.emplace(p.first, *signed_value);
    }
  }
  std::ostringstream oss;
  for (auto& p : bindings) {
    if (p.first != bindings.begin()->first) {
      oss << ",";
    }
    oss << p.first << ":";
    auto& d = p.second;
    if (d.get_constant()) {
      oss << *d.get_constant();
    } else {
      oss << d.interval();
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
          m_call_constant_arguments.emplace(insn, constant_arguments);
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
  // be inlined, it's code will no longer change. So we can cache...
  // - its size
  // - its set of type refs
  // - its set of method refs
  // - whether all callers are in the same class, and are called from how many
  //   classes
  m_callee_insn_sizes =
      std::make_unique<ConcurrentMap<const DexMethod*, size_t>>();
  m_callee_type_refs = std::make_unique<
      ConcurrentMap<const DexMethod*, std::vector<DexType*>>>();
  m_callee_method_refs =
      std::make_unique<ConcurrentMap<const DexMethod*, size_t>>();
  m_callee_caller_refs =
      std::make_unique<ConcurrentMap<const DexMethod*, CalleeCallerRefs>>();

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
      always_assert(!callees.empty());
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
  always_assert(!callees.empty());

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
    } else if (m_inline_for_speed.should_inline(caller, callee)) {
      nonrecursive_callees.push_back(callee);
    }

    stack_depth = std::max(stack_depth, callee_stack_depth + 1);
  }

  (*visited)[caller] = stack_depth;
  if (!nonrecursive_callees.empty()) {
    (*caller_nonrecursive_callees_by_stack_depth)[stack_depth].push_back(
        std::make_pair(caller, nonrecursive_callees));
  }
  return stack_depth;
}

void MultiMethodInliner::caller_inline(
    DexMethod* caller, const std::vector<DexMethod*>& nonrecursive_callees) {
  // We select callees to inline into this caller
  std::vector<DexMethod*> selected_callees;
  std::vector<DexMethod*> optional_selected_callees;
  selected_callees.reserve(nonrecursive_callees.size());
  for (auto callee : nonrecursive_callees) {
    if (should_inline(callee)) {
      selected_callees.push_back(callee);
    } else {
      optional_selected_callees.push_back(callee);
    }
  }

  if (!selected_callees.empty() || !optional_selected_callees.empty()) {
    inline_callees(caller, selected_callees, optional_selected_callees);
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
  auto initial_env = constant_propagation::interprocedural::env_with_params(
      is_static(caller), code, ConstantArguments());
  intra_cp.run(initial_env);
  for (const auto& block : cfg.blocks()) {
    auto env = intra_cp.get_entry_state_at(block);
    if (env.is_bottom()) {
      res.dead_blocks++;
      // we found an unreachable block; ignore invoke instructions in it
      continue;
    }
    auto last_insn = block->get_last_insn();
    for (auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      if (opcode::is_an_invoke(insn->opcode())) {
        auto callee = resolver(insn->get_method(), opcode_to_search(insn));
        if (callees_set.count(callee)) {
          ConstantArguments constant_arguments;
          const auto& srcs = insn->srcs();
          for (size_t i = is_static(callee) ? 0 : 1; i < srcs.size(); ++i) {
            auto val = env.get(srcs[i]);
            always_assert(!val.is_bottom());
            constant_arguments.set(i, val);
          }
          res.invoke_constant_arguments.emplace_back(code->iterator_to(mie),
                                                     constant_arguments);
        }
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

void MultiMethodInliner::inline_callees(
    DexMethod* caller,
    const std::vector<DexMethod*>& callees,
    const std::vector<DexMethod*>& optional_callees) {
  size_t found = 0;

  // walk the caller opcodes collecting all candidates to inline
  // Build a callee to opcode map
  std::vector<Inlinable> inlinables;
  auto max = callees.size() + optional_callees.size();
  editable_cfg_adapter::iterate_with_iterator(
      caller->get_code(), [&](const IRList::iterator& it) {
        auto insn = it->insn;
        if (!opcode::is_an_invoke(insn->opcode())) {
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
        bool optional{false};
        if (std::find(callees.begin(), callees.end(), callee) ==
            callees.end()) {
          // If a callee wasn't in the list of general callees, that means that
          // it's not beneficial on average (across all callsites) to inline the
          // callee. However, let's see if it's beneficial for this particular
          // callsite, taking into account constant-arguments (if any).
          if (std::find(optional_callees.begin(), optional_callees.end(),
                        callee) != optional_callees.end() &&
              should_inline_optional(caller, insn, callee)) {
            optional = true;
          } else {
            return editable_cfg_adapter::LOOP_CONTINUE;
          }
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

        inlinables.push_back((Inlinable){callee, it, insn, optional});
        if (++found == max) {
          return editable_cfg_adapter::LOOP_BREAK;
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
  if (found != max) {
    always_assert(found <= max);
    info.not_found += max - found;
  }

  if (!inlinables.empty()) {
    inline_inlinables(caller, inlinables);
  }
}

void MultiMethodInliner::inline_callees(
    DexMethod* caller, const std::unordered_set<IRInstruction*>& insns) {
  std::vector<Inlinable> inlinables;
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
          inlinables.push_back((Inlinable){callee, it, insn});
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
    DexMethod* caller_method, const std::vector<Inlinable>& inlinables) {

  auto caller = caller_method->get_code();
  std::unordered_set<IRCode*> need_deconstruct;
  if (inline_inlinables_need_deconstruct(caller_method)) {
    need_deconstruct.reserve(1 + inlinables.size());
    need_deconstruct.insert(caller);
    for (const auto& inlinable : inlinables) {
      need_deconstruct.insert(inlinable.callee->get_code());
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
  std::vector<Inlinable> ordered_inlinables(inlinables.begin(),
                                            inlinables.end());

  std::stable_sort(ordered_inlinables.begin(), ordered_inlinables.end(),
                   [&](const Inlinable& a, const Inlinable& b) {
                     // First, prefer non-optional inlinables, as they were
                     // (potentially) selected with a global cost model, taking
                     // into account the savings achieved when eliminating all
                     // calls
                     if (a.optional != b.optional) {
                       return a.optional < b.optional;
                     }
                     // Second, prefer smaller methods, to avoid hitting size
                     // limits too soon
                     return get_callee_insn_size(a.callee) <
                            get_callee_insn_size(b.callee);
                   });

  std::vector<DexMethod*> inlined_callees;
  boost::optional<reg_t> cfg_next_caller_reg;
  if (m_config.use_cfg_inliner && !m_config.unique_inlined_registers) {
    cfg_next_caller_reg = caller->cfg().get_registers_size();
  }
  size_t calls_not_inlinable{0}, calls_not_inlined{0};

  std::unique_ptr<ab_test::ABExperimentContext> exp;
  bool caller_had_editable_cfg = caller->editable_cfg_built();

  if (for_speed()) {
    if (!caller_had_editable_cfg) {
      caller->build_cfg();
    }
    exp = ab_test::ABExperimentContext::create(&caller->cfg(), caller_method,
                                               "pgi_v1");
  }

  for (const auto& inlinable : ordered_inlinables) {
    auto callee_method = inlinable.callee;
    auto callee = callee_method->get_code();
    auto callsite_insn = inlinable.insn;

    std::vector<DexMethod*> make_static;
    if (!is_inlinable(caller_method, callee_method, callsite_insn,
                      estimated_insn_size, &make_static)) {
      calls_not_inlinable++;
      continue;
    }
    // Only now, when are about actually inline the method, we'll record
    // the fact that we'll have to make some methods static.
    make_static_inlinable(make_static);

    TRACE(MMINL, 4, "inline %s (%d) in %s (%d)", SHOW(callee),
          caller->get_registers_size(), SHOW(caller),
          callee->get_registers_size());

    if (m_config.use_cfg_inliner) {
      if (m_config.unique_inlined_registers) {
        cfg_next_caller_reg = caller->cfg().get_registers_size();
      }
      bool success = inliner::inline_with_cfg(
          caller_method, callee_method, callsite_insn, *cfg_next_caller_reg);
      if (!success) {
        calls_not_inlined++;
        continue;
      }
    } else {
      // Logging before the call to inline_method to get the most relevant line
      // number near callsite before callsite gets replaced. Should be ok as
      // inline_method does not fail to inline.
      log_opt(INLINED, caller_method, callsite_insn);

      auto callsite = inlinable.iterator;
      always_assert(callsite->insn == callsite_insn);
      inliner::inline_method_unsafe(caller_method, caller, callee, callsite);
    }
    TRACE(INL, 2, "caller: %s\tcallee: %s", SHOW(caller), SHOW(callee));
    estimated_insn_size += get_callee_insn_size(callee_method);

    inlined_callees.push_back(callee_method);
  }

  if (!inlined_callees.empty()) {
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

  if (exp != nullptr) {
    exp->flush();
    if (caller_had_editable_cfg) {
      caller->build_cfg();
    }
  }

  info.calls_inlined += inlined_callees.size();
  if (calls_not_inlinable) {
    info.calls_not_inlinable += calls_not_inlinable;
  }
  if (calls_not_inlined) {
    info.calls_not_inlined += calls_not_inlined;
  }
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
    {
      constant_propagation::intraprocedural::FixpointIterator fp_iter(
          code->cfg(), constant_propagation::ConstantPrimitiveAnalyzer());
      fp_iter.run(ConstantEnvironment());
      constant_propagation::Transform::Config config;
      constant_propagation::Transform tf(config);
      const_prop_stats = tf.apply_on_uneditable_cfg(
          fp_iter, constant_propagation::WholeProgramState(), code, &xstores,
          method->get_class());
    }
    always_assert(!code->editable_cfg_built());
    code->build_cfg(/* editable */ true);
    code->cfg().calculate_exit_block();
    {
      constant_propagation::intraprocedural::FixpointIterator fp_iter(
          code->cfg(), constant_propagation::ConstantPrimitiveAnalyzer());
      fp_iter.run(ConstantEnvironment());
      constant_propagation::Transform::Config config;
      constant_propagation::Transform tf(config);
      const_prop_stats += tf.apply(fp_iter, code->cfg(), method, &xstores);
    }
  }

  if (m_config.run_cse) {
    if (!code->editable_cfg_built()) {
      code->build_cfg(/* editable */ true);
    }

    cse_impl::CommonSubexpressionElimination cse(
        m_cse_shared_state.get(), code->cfg(), is_static(method),
        method::is_init(method) || method::is_clinit(method),
        method->get_class(), method->get_proto()->get_args());
    cse.patch();
    cse_stats = cse.get_stats();
  }

  if (m_config.run_copy_prop) {
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
    dedup_blocks_impl::DedupBlocks dedup_blocks(&config, method);
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

  // This pre-populates the
  // - m_should_inline,
  // - m_callee_insn_sizes, and
  // - m_callee_type_refs
  // - m_callee_caller_refs
  // - m_callee_method_refs
  // caches.
  if (should_inline(method)) {
    get_callee_insn_size(method);
    get_callee_type_refs(method);
    if (m_mode != IntraDex && !is_private(method) &&
        !get_callee_caller_refs(method).same_class) {
      get_callee_method_refs(method);
    }
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
bool MultiMethodInliner::is_inlinable(const DexMethod* caller,
                                      const DexMethod* callee,
                                      const IRInstruction* insn,
                                      size_t estimated_insn_size,
                                      std::vector<DexMethod*>* make_static) {
  // don't inline cross store references
  if (cross_store_reference(caller, callee)) {
    if (insn) {
      log_nopt(INL_CROSS_STORE_REFS, caller, insn);
    }
    return false;
  }
  if (is_blocklisted(callee)) {
    if (insn) {
      log_nopt(INL_BLOCK_LISTED_CALLEE, callee);
    }
    return false;
  }
  if (caller_is_blocklisted(caller)) {
    if (insn) {
      log_nopt(INL_BLOCK_LISTED_CALLER, caller);
    }
    return false;
  }
  if (has_external_catch(callee)) {
    if (insn) {
      log_nopt(INL_EXTERN_CATCH, callee);
    }
    return false;
  }
  if (cannot_inline_opcodes(caller, callee, insn, make_static)) {
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

  return true;
}

void MultiMethodInliner::make_static_inlinable(
    std::vector<DexMethod*>& make_static) {
  for (auto method : make_static) {
    m_delayed_make_static.insert(method);
  }
}

/**
 * Return whether the method or any of its ancestors are in the blocklist.
 * Typically used to prevent inlining / deletion of methods that are called
 * via reflection.
 */
bool MultiMethodInliner::is_blocklisted(const DexMethod* callee) {
  auto cls = type_class(callee->get_class());
  // Enums' kept methods are all excluded.
  if (is_enum(cls) && root(callee)) {
    return true;
  }
  while (cls != nullptr) {
    if (m_config.get_blocklist().count(cls->get_type())) {
      info.blocklisted++;
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

  if (m_config.allowlist_no_method_limit.count(caller_type)) {
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
    const auto absent = std::numeric_limits<size_t>::max();
    auto size = m_callee_insn_sizes->get(callee, absent);
    if (size != absent) {
      return size;
    }
  }

  const IRCode* code = callee->get_code();
  auto size = code->editable_cfg_built() ? code->cfg().sum_opcode_sizes()
                                         : code->sum_opcode_sizes();
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

static size_t get_invoke_cost(const DexMethod* callee) {
  size_t invoke_cost = callee->get_proto()->is_void()
                           ? COST_INVOKE_WITHOUT_RESULT
                           : COST_INVOKE_WITH_RESULT;
  invoke_cost += get_inlined_regs_cost(callee->get_proto()->get_args()->size());
  return invoke_cost;
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
  if (!opcode::is_an_internal(op) && !opcode::is_a_move(op) &&
      !opcode::is_a_return(op)) {
    cost++;
    auto regs = insn->srcs_size() +
                ((insn->has_dest() || insn->has_move_result_pseudo()) ? 1 : 0);
    cost += get_inlined_regs_cost(regs);
    if (op == OPCODE_MOVE_EXCEPTION) {
      cost += 8; // accounting for book-keeping overhead of throw-blocks
    } else if (insn->has_method() || insn->has_field() || insn->has_type() ||
               insn->has_string() || opcode::is_a_conditional_branch(op)) {
      cost++;
    } else if (insn->has_data()) {
      cost += 4 + insn->get_data()->size();
    } else if (insn->has_literal()) {
      auto lit = insn->get_literal();
      if (lit < -2147483648 || lit > 2147483647) {
        cost += 4;
      } else if (lit < -32768 || lit > 32767) {
        cost += 2;
      } else if (opcode::is_a_const(op) && (lit < -8 || lit > 7)) {
        cost++;
      } else if (!opcode::is_a_const(op) && (lit < -128 || lit > 127)) {
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
    auto target = block->goes_to();
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
    bool is_static,
    const IRCode* code,
    const ConstantArguments* constant_arguments = nullptr) {
  auto& cfg = code->cfg();

  size_t cost{0};
  // For each unreachable block, we'll give a small discount to
  // reflect the fact that some incoming branch instruction also will get
  // simplified or eliminated.
  size_t conditional_branch_cost_discount{0};
  size_t dead_blocks{0};
  size_t returns{0};
  if (code->editable_cfg_built()) {
    std::unique_ptr<constant_propagation::intraprocedural::FixpointIterator>
        intra_cp;
    if (constant_arguments && !constant_arguments->is_top()) {
      intra_cp.reset(
          new constant_propagation::intraprocedural::FixpointIterator(
              cfg, constant_propagation::ConstantPrimitiveAnalyzer()));
      ConstantEnvironment initial_env =
          constant_propagation::interprocedural::env_with_params(
              is_static, code, *constant_arguments);
      intra_cp->run(initial_env);
    }

    const auto blocks = code->cfg().blocks();
    for (size_t i = 0; i < blocks.size(); ++i) {
      auto block = blocks.at(i);
      if (intra_cp && intra_cp->get_entry_state_at(block).is_bottom()) {
        // we found an unreachable block
        for (const auto edge : block->preds()) {
          const auto& pred_env = intra_cp->get_entry_state_at(edge->src());
          if (!pred_env.is_bottom() && edge->type() == cfg::EDGE_BRANCH) {
            // A conditional branch instruction is going to disppear
            // TODO: Simulate backwards LocalDCE run to find what instructions
            // actually remain live
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
        if (opcode::is_a_return(insn->opcode())) {
          returns++;
        }
      }
    }
  } else {
    editable_cfg_adapter::iterate(code, [&](const MethodItemEntry& mie) {
      auto insn = mie.insn;
      cost += get_inlined_cost(insn);
      if (opcode::is_a_return(insn->opcode())) {
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
  std::atomic<size_t> inlined_cost{
      ::get_inlined_cost(is_static(callee), callee->get_code()).cost};
  ConcurrentMap<std::string, size_t> inlined_costs_keyed;
  auto callee_constant_arguments_it = m_callee_constant_arguments.find(callee);
  if (callee_constant_arguments_it != m_callee_constant_arguments.end() &&
      inlined_cost <= MAX_COST_FOR_CONSTANT_PROPAGATION) {
    const auto& callee_constant_arguments =
        callee_constant_arguments_it->second;
    auto process_key = [&](const ConstantArgumentsOccurrences& cao) {
      const auto& constant_arguments = cao.first;
      const auto count = cao.second;
      TRACE(INLINE, 5, "[too_many_callers] get_inlined_cost %s", SHOW(callee));
      auto res = ::get_inlined_cost(is_static(callee), callee->get_code(),
                                    &constant_arguments);
      TRACE(INLINE, 4,
            "[too_many_callers] get_inlined_cost with %zu constant invoke "
            "params %s @ %s: cost %zu (dead blocks: %zu)",
            constant_arguments.is_top() ? 0 : constant_arguments.size(),
            get_key(constant_arguments).c_str(), SHOW(callee), res.cost,
            res.dead_blocks);
      callees_unreachable_blocks += res.dead_blocks * count;
      inlined_cost += res.cost * count;
      callees_analyzed += count;
      inlined_costs_keyed.emplace(get_key(constant_arguments), res.cost);
    };

    if (callee_constant_arguments.size() > 1 &&
        callee_constant_arguments.size() * inlined_cost >=
            MIN_COST_FOR_PARALLELIZATION) {
      // TODO: This parallelization happens independently (in addition to) the
      // parallelization via m_async_method_executor. This should be combined,
      // using a single thread pool.
      inlined_cost = 0;
      auto num_threads = std::min(redex_parallel::default_num_threads(),
                                  callee_constant_arguments.size());
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
    m_inlined_costs_keyed.emplace(
        callee, std::make_shared<std::unordered_map<std::string, size_t>>(
                    inlined_costs_keyed.begin(), inlined_costs_keyed.end()));
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
  if (m_same_method_implementations == nullptr) {
    return 1;
  }

  auto it = m_same_method_implementations->find(callee);
  if (it != m_same_method_implementations->end()) {
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

  boost::optional<CalleeCallerRefs> callee_caller_refs;
  size_t cross_dex_penalty{0};
  if (m_mode != IntraDex && !is_private(callee)) {
    callee_caller_refs = get_callee_caller_refs(callee);
    if (callee_caller_refs->same_class) {
      callee_caller_refs = boost::none;
    } else {
      // Inlining methods into different classes might lead to worse
      // cross-dex-ref minimization results.
      cross_dex_penalty = get_callee_method_refs(callee);
      if (callee_caller_refs->classes > 1) {
        cross_dex_penalty++;
      }
      inlined_cost += cross_dex_penalty;
    }
  }

  // 2. Determine costs of keeping the invoke instruction

  size_t invoke_cost = get_invoke_cost(callee);
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

  std::unordered_set<DexMethod*> callers_set(callers.begin(), callers.end());

  // Can we inline the init-callee into all callers?
  // If not, then we can give up, as there's no point in making the case that
  // we can eliminate the callee method based on pervasive inlining.
  if (m_analyze_and_prune_inits && method::is_init(callee)) {
    if (!callee->get_code()->editable_cfg_built()) {
      return true;
    }
    if (!can_inline_init(callee)) {
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
    size_t classes = callee_caller_refs ? callee_caller_refs->classes : 0;

    // The cost of keeping a method amounts of somewhat fixed metadata overhead,
    // plus the method body, which we approximate with the inlined cost.
    size_t method_cost = COST_METHOD + get_inlined_cost(callee);
    auto methods_cost = method_cost * same_method_implementations;

    // If we inline invocations to this method everywhere, we could delete the
    // method. Is this worth it, given the number of callsites and costs
    // involved?
    if ((inlined_cost - cross_dex_penalty) * caller_count +
            classes * cross_dex_penalty >
        invoke_cost * caller_count + methods_cost) {
      return true;
    }

    // We can't eliminate the method entirely if it's not inlinable
    for (auto caller : callers_set) {
      if (!is_inlinable(caller, callee, /* insn */ nullptr,
                        /* estimated_insn_size */ 0,
                        /* make_static */ nullptr)) {
        return true;
      }
    }

    return false;
  }

  return true;
}

bool MultiMethodInliner::should_inline_optional(
    DexMethod* caller, const IRInstruction* invoke_insn, DexMethod* callee) {
  if (!m_call_constant_arguments.count_unsafe(invoke_insn)) {
    return false;
  }
  auto& constant_arguments = m_call_constant_arguments.at_unsafe(invoke_insn);
  auto opt_inlined_costs_keyed = m_inlined_costs_keyed.get(
      callee, std::shared_ptr<std::unordered_map<std::string, size_t>>());
  if (!opt_inlined_costs_keyed) {
    return false;
  }
  auto key = get_key(constant_arguments);
  auto it = opt_inlined_costs_keyed->find(key);
  if (it == opt_inlined_costs_keyed->end()) {
    return false;
  }
  auto inlined_cost = it->second;

  if (m_mode != IntraDex && !is_private(callee) &&
      caller->get_class() != callee->get_class()) {
    // Inlining methods into different classes might lead to worse
    // cross-dex-ref minimization results.
    inlined_cost += 1 + get_callee_method_refs(callee);
  }

  size_t invoke_cost = get_invoke_cost(callee);
  if (inlined_cost > invoke_cost) {
    return false;
  }

  return true;
}

bool MultiMethodInliner::caller_is_blocklisted(const DexMethod* caller) {
  auto cls = caller->get_class();
  if (m_config.get_caller_blocklist().count(cls)) {
    info.blocklisted++;
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
        if (opcode::is_a_return(insn->opcode())) {
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
      if (make_static) {
        make_static->push_back(method);
      }
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
  if (opcode::is_an_ifield_op(insn->opcode()) ||
      opcode::is_an_sfield_op(insn->opcode())) {
    auto ref = insn->get_field();
    DexField* field = resolve_field(ref, opcode::is_an_sfield_op(insn->opcode())
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
  if (opcode::is_an_sget(op)) {
    auto ref = insn->get_field();
    DexField* field = resolve_field(ref, FieldSearch::Static);
    if (field != nullptr && field == m_sdk_int_field) {
      return true;
    }
  }
  return false;
}

std::vector<DexType*> MultiMethodInliner::get_callee_type_refs(
    const DexMethod* callee) {
  if (m_callee_type_refs) {
    auto absent = std::vector<DexType*>{nullptr};
    auto cached = m_callee_type_refs->get(callee, absent);
    if (cached != absent) {
      return cached;
    }
  }

  std::unordered_set<DexType*> type_refs_set;
  editable_cfg_adapter::iterate(
      callee->get_code(), [&](const MethodItemEntry& mie) {
        auto insn = mie.insn;
        if (insn->has_type()) {
          type_refs_set.insert(insn->get_type());
        } else if (insn->has_method()) {
          auto meth = insn->get_method();
          type_refs_set.insert(meth->get_class());
          auto proto = meth->get_proto();
          type_refs_set.insert(proto->get_rtype());
          auto args = proto->get_args();
          if (args != nullptr) {
            for (const auto& arg : args->get_type_list()) {
              type_refs_set.insert(arg);
            }
          }
        } else if (insn->has_field()) {
          auto field = insn->get_field();
          type_refs_set.insert(field->get_class());
          type_refs_set.insert(field->get_type());
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });

  std::vector<DexType*> type_refs;
  for (auto type : type_refs_set) {
    // filter out what xstores.illegal_ref(...) doesn't care about
    if (type_class_internal(type) == nullptr) {
      continue;
    }
    type_refs.push_back(type);
  }

  if (m_callee_type_refs) {
    m_callee_type_refs->emplace(callee, type_refs);
  }
  return type_refs;
}

size_t MultiMethodInliner::get_callee_method_refs(const DexMethod* callee) {
  if (m_callee_method_refs) {
    auto absent = std::numeric_limits<size_t>::max();
    auto cached = m_callee_method_refs->get(callee, absent);
    if (cached != absent) {
      return cached;
    }
  }

  std::unordered_set<DexMethodRef*> method_refs_set;
  editable_cfg_adapter::iterate(
      callee->get_code(), [&](const MethodItemEntry& mie) {
        auto insn = mie.insn;
        if (insn->has_method()) {
          auto method = insn->get_method();
          auto cls = type_class_internal(method->get_class());
          if (cls) {
            method_refs_set.insert(method);
          }
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });

  if (m_callee_method_refs) {
    m_callee_method_refs->emplace(callee, method_refs_set.size());
  }
  return method_refs_set.size();
}

CalleeCallerRefs MultiMethodInliner::get_callee_caller_refs(
    const DexMethod* callee) {
  if (m_callee_caller_refs) {
    CalleeCallerRefs absent = {false, std::numeric_limits<size_t>::max()};
    auto cached = m_callee_caller_refs->get(callee, absent);
    if (cached.classes != absent.classes) {
      return cached;
    }
  }

  const auto& callers = callee_caller.at(callee);
  std::unordered_set<DexType*> caller_classes;
  for (auto caller : callers) {
    caller_classes.insert(caller->get_class());
  }
  auto callee_class = callee->get_class();
  CalleeCallerRefs callee_caller_refs{
      /* same_class */ caller_classes.size() == 1 &&
          *caller_classes.begin() == callee_class,
      caller_classes.size()};

  if (m_callee_caller_refs) {
    m_callee_caller_refs->emplace(callee, callee_caller_refs);
  }
  return callee_caller_refs;
}

bool MultiMethodInliner::cross_store_reference(const DexMethod* caller,
                                               const DexMethod* callee) {
  auto callee_type_refs = get_callee_type_refs(callee);
  size_t store_idx = xstores.get_store_idx(caller->get_class());
  for (auto type : callee_type_refs) {
    if (xstores.illegal_ref(store_idx, type)) {
      info.cross_store++;
      return true;
    }
  }

  return false;
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
          opcode::is_a_load_param(it->insn->opcode())) {
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
      FALLTHROUGH_INTENDED;
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
  change_visibility(callee_code, caller->get_class(), caller);
  inline_method_unsafe(caller, caller->get_code(), callee_code, pos);
}

void inline_method_unsafe(const DexMethod* caller_method,
                          IRCode* caller_code,
                          IRCode* callee_code,
                          const IRList::iterator& pos) {
  TRACE(INL, 5, "caller code:\n%s", SHOW(caller_code));
  TRACE(INL, 5, "callee code:\n%s", SHOW(callee_code));

  if (caller_code->get_debug_item() == nullptr && caller_method != nullptr) {
    // Create an empty item so that debug info of inlinee does not get lost.
    caller_code->set_debug_item(std::make_unique<DexDebugItem>());
    // Create a fake position.
    auto it = caller_code->main_block();
    if (it != caller_code->end()) {
      caller_code->insert_after(
          caller_code->main_block(),
          *(new MethodItemEntry(
              DexPosition::make_synthetic_entry_position(caller_method))));
    } else {
      caller_code->push_back(*(new MethodItemEntry(
          DexPosition::make_synthetic_entry_position(caller_method))));
    }
  }

  auto callee_reg_map = gen_callee_reg_map(caller_code, callee_code, pos);

  // find the move-result after the invoke, if any. Must be the first
  // instruction after the invoke
  auto move_res = pos;
  while (move_res++ != caller_code->end() && move_res->type != MFLOW_OPCODE)
    ;
  if (!opcode::is_a_move_result(move_res->insn->opcode())) {
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
        return mei.type == MFLOW_OPCODE &&
               opcode::is_a_return(mei.insn->opcode());
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

  callee_code->cleanup_debug();
  auto it = callee_code->begin();
  while (it != callee_code->end()) {
    auto& mei = *it++;
    if (mei.type == MFLOW_OPCODE &&
        opcode::is_a_load_param(mei.insn->opcode())) {
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

namespace impl {

struct BlockAccessor {
  static void push_dex_pos(cfg::Block* b,
                           std::unique_ptr<DexPosition> dex_pos) {
    auto it = b->get_first_non_param_loading_insn();
    auto mie = new MethodItemEntry(std::move(dex_pos));
    if (it == b->end()) {
      b->m_entries.push_back(*mie);
    } else {
      b->m_entries.insert_before(it, *mie);
    }
  }
};

} // namespace impl

// return true on successful inlining, false otherwise
bool inline_with_cfg(DexMethod* caller_method,
                     DexMethod* callee_method,
                     IRInstruction* callsite,
                     size_t next_caller_reg) {

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

  if (caller_code->get_debug_item() == nullptr) {
    // Create an empty item so that debug info of inlinee does not get lost.
    caller_code->set_debug_item(std::make_unique<DexDebugItem>());
    // Create a fake position.
    impl::BlockAccessor::push_dex_pos(
        caller_cfg.entry_block(),
        DexPosition::make_synthetic_entry_position(caller_method));
  }

  // Logging before the call to inline_cfg to get the most relevant line
  // number near callsite before callsite gets replaced. Should be ok as
  // inline_cfg does not fail to inline.
  log_opt(INLINED, caller_method, callsite);

  auto callee_code = callee_method->get_code();
  always_assert(callee_code->editable_cfg_built());
  cfg::CFGInliner::inline_cfg(&caller_cfg, callsite_it, callee_code->cfg(),
                              next_caller_reg);

  return true;
}

} // namespace inliner
