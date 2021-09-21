/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Inliner.h"

#include <cstdint>
#include <utility>

#include "ApiLevelChecker.h"
#include "CFGInliner.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationWholeProgramState.h"
#include "ConstructorAnalysis.h"
#include "DexInstruction.h"
#include "EditableCfgAdapter.h"
#include "GraphUtil.h"
#include "InlineForSpeed.h"
#include "InlinerConfig.h"
#include "LocalDce.h"
#include "LoopInfo.h"
#include "Macros.h"
#include "MethodProfiles.h"
#include "MonitorCount.h"
#include "Mutators.h"
#include "OptData.h"
#include "OutlinedMethods.h"
#include "RecursionPruner.h"
#include "StlUtil.h"
#include "Timer.h"
#include "UnknownVirtuals.h"
#include "Walkers.h"
#include "WorkQueue.h"

using namespace opt_metadata;
using namespace outliner;

namespace {

// The following costs are in terms of code-units (2 bytes).

// Typical overhead of calling a method, without move-result overhead.
const float COST_INVOKE = 3.7f;

// Typical overhead of having move-result instruction.
const float COST_MOVE_RESULT = 3.0f;

// Overhead of having a method and its metadata.
const size_t COST_METHOD = 16;

// When to consider running constant-propagation to better estimate inlined
// cost. It just takes too much time to run the analysis for large methods.
const size_t MAX_COST_FOR_CONSTANT_PROPAGATION = 1000;

/*
 * This is the maximum size of method that Dex bytecode can encode.
 * The table of instructions is indexed by a 32 bit unsigned integer.
 */
constexpr uint64_t HARD_MAX_INSTRUCTION_SIZE = UINT64_C(1) << 32;

} // namespace

MultiMethodInliner::MultiMethodInliner(
    const std::vector<DexClass*>& scope,
    DexStoresVector& stores,
    const std::unordered_set<DexMethod*>& candidates,
    std::function<DexMethod*(DexMethodRef*, MethodSearch)>
        concurrent_resolve_fn,
    const inliner::InlinerConfig& config,
    MultiMethodInlinerMode mode /* default is InterDex */,
    const CalleeCallerInsns& true_virtual_callers,
    InlineForSpeed* inline_for_speed,
    bool analyze_and_prune_inits,
    const std::unordered_set<DexMethodRef*>& configured_pure_methods,
    const api::AndroidSDK* min_sdk_api,
    const std::unordered_set<DexString*>& configured_finalish_field_names)
    : m_concurrent_resolver(std::move(concurrent_resolve_fn)),
      m_scheduler(
          [this](DexMethod* method) {
            auto it = caller_callee.find(method);
            if (it != caller_callee.end()) {
              always_assert(!inline_inlinables_need_deconstruct(method));
              std::unordered_set<DexMethod*> callees;
              for (auto& p : it->second) {
                callees.insert(p.first);
              }
              inline_callees(method, callees,
                             /* filter_via_should_inline */ true);
              // We schedule the post-processing, to allow other unlocked
              // higher-priority tasks take precedence
              m_scheduler.augment(
                  method, [this, method] { postprocess_method(method); });
            } else {
              postprocess_method(method);
            }
          },
          0),
      m_scope(scope),
      m_config(config),
      m_mode(mode),
      m_inline_for_speed(inline_for_speed),
      m_analyze_and_prune_inits(analyze_and_prune_inits),
      m_shrinker(stores,
                 scope,
                 config.shrinker,
                 configured_pure_methods,
                 configured_finalish_field_names) {
  Timer t("MultiMethodInliner construction");
  for (const auto& callee_callers : true_virtual_callers) {
    auto callee = callee_callers.first;
    if (callee_callers.second.other_call_sites) {
      m_true_virtual_callees_with_other_call_sites.insert(callee);
    }
    for (const auto& caller_insns : callee_callers.second.caller_insns) {
      for (auto insn : caller_insns.second) {
        caller_virtual_callee[caller_insns.first][insn] = callee;
      }
      auto& iinc = callee_callers.second.inlined_invokes_need_cast;
      m_inlined_invokes_need_cast.insert(iinc.begin(), iinc.end());
    }
  }
  // Walk every opcode in scope looking for calls to inlinable candidates and
  // build a map of callers to callees and the reverse callees to callers. If
  // mode != IntraDex, we build the map for all the candidates. If mode ==
  // IntraDex, we properly exclude invocations where the caller is located in
  // another dex from the callee, and remember all such x-dex callees.
  ConcurrentSet<DexMethod*> concurrent_x_dex_callees;
  ConcurrentMap<const DexMethod*, std::unordered_map<DexMethod*, size_t>>
      concurrent_callee_caller;
  ConcurrentMap<DexMethod*, std::unordered_map<DexMethod*, size_t>>
      concurrent_caller_callee;
  std::unique_ptr<XDexRefs> x_dex;
  if (mode == IntraDex) {
    x_dex = std::make_unique<XDexRefs>(stores);
  }
  if (min_sdk_api) {
    const auto& xstores = m_shrinker.get_xstores();
    m_ref_checkers =
        std::make_unique<std::vector<std::unique_ptr<RefChecker>>>();
    for (size_t store_idx = 0; store_idx < xstores.size(); store_idx++) {
      m_ref_checkers->emplace_back(
          std::make_unique<RefChecker>(&xstores, store_idx, min_sdk_api));
    }
  }

  walk::parallel::opcodes(
      scope, [](DexMethod*) { return true; },
      [&](DexMethod* caller, IRInstruction* insn) {
        if (!opcode::is_an_invoke(insn->opcode())) {
          return;
        }
        auto callee =
            m_concurrent_resolver(insn->get_method(), opcode_to_search(insn));
        if (callee == nullptr || !callee->is_concrete() ||
            !candidates.count(callee) || true_virtual_callers.count(callee)) {
          return;
        }
        if (x_dex && x_dex->cross_dex_ref(caller, callee)) {
          concurrent_x_dex_callees.insert(callee);
          return;
        }
        concurrent_callee_caller.update(
            callee,
            [caller](const DexMethod*,
                     std::unordered_map<DexMethod*, size_t>& v,
                     bool) { ++v[caller]; });
        concurrent_caller_callee.update(
            caller,
            [callee](const DexMethod*,
                     std::unordered_map<DexMethod*, size_t>& v,
                     bool) { ++v[callee]; });
      });
  m_x_dex_callees.insert(concurrent_x_dex_callees.begin(),
                         concurrent_x_dex_callees.end());
  callee_caller.insert(concurrent_callee_caller.begin(),
                       concurrent_callee_caller.end());
  caller_callee.insert(concurrent_caller_callee.begin(),
                       concurrent_caller_callee.end());
  for (const auto& callee_callers : true_virtual_callers) {
    auto callee = callee_callers.first;
    for (const auto& caller_insns : callee_callers.second.caller_insns) {
      auto caller = const_cast<DexMethod*>(caller_insns.first);
      if (x_dex && x_dex->cross_dex_ref(caller, callee)) {
        m_x_dex_callees.insert(callee);
        continue;
      }
      ++callee_caller[callee][caller];
      ++caller_callee[caller][callee];
    }
  }
  if (inline_for_speed && !m_x_dex_callees.empty()) {
    // pruning of any (caller, callee) pair if callee is involved in any x-dex
    // caller/callee relationship
    // TODO: This is to maintain the old IntraDex behavior for the
    // PerfMethodInlinePass; evaluate if this is the best behavior for the
    // PerfMethodInlinePass.
    for (auto callee : m_x_dex_callees) {
      callee_caller.erase(callee);
    }
    for (auto& p : caller_callee) {
      std20::erase_if(p.second,
                      [&](auto& q) { return m_x_dex_callees.count(q.first); });
    }
    std20::erase_if(caller_callee, [&](auto& p) { return p.second.empty(); });
  }
}

void MultiMethodInliner::inline_methods(bool methods_need_deconstruct) {
  std::unordered_set<IRCode*> need_deconstruct;
  if (methods_need_deconstruct) {
    for (auto& p : caller_callee) {
      need_deconstruct.insert(const_cast<IRCode*>(p.first->get_code()));
    }
    for (auto& p : callee_caller) {
      need_deconstruct.insert(const_cast<IRCode*>(p.first->get_code()));
    }
    for (auto it = need_deconstruct.begin(); it != need_deconstruct.end();) {
      if ((*it)->editable_cfg_built()) {
        it = need_deconstruct.erase(it);
      } else {
        it++;
      }
    }
    if (!need_deconstruct.empty()) {
      workqueue_run<IRCode*>(
          [](IRCode* code) { code->build_cfg(/* editable */ true); },
          need_deconstruct);
    }
  }

  m_ab_experiment_context = ab_test::ABExperimentContext::create("pgi_v1");

  // Inlining and shrinking initiated from within this method will be done
  // in parallel.
  m_scheduler.get_thread_pool().set_num_threads(
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
  m_callee_type_refs =
      std::make_unique<ConcurrentMap<const DexMethod*,
                                     std::shared_ptr<std::vector<DexType*>>>>();
  if (m_ref_checkers) {
    m_callee_code_refs = std::make_unique<
        ConcurrentMap<const DexMethod*, std::shared_ptr<CodeRefs>>>();
  }
  m_callee_caller_refs =
      std::make_unique<ConcurrentMap<const DexMethod*, CalleeCallerRefs>>();

  // Instead of changing visibility as we inline, blocking other work on the
  // critical path, we do it all in parallel at the end.
  m_delayed_visibility_changes = std::make_unique<VisibilityChanges>();

  // we want to inline bottom up, so as a first step, for all callers, we
  // recurse into all inlinable callees until we hit a leaf and we start
  // inlining from there. First, we just gather data on
  // caller/non-recursive-callees pairs for each stack depth.
  {
    auto exclude_fn = [this](DexMethod* caller, DexMethod* callee) {
      return for_speed() &&
             !m_inline_for_speed->should_inline_generic(caller, callee);
    };
    inliner::RecursionPruner recursion_pruner(callee_caller, caller_callee,
                                              std::move(exclude_fn));
    recursion_pruner.run();
    info.recursive = recursion_pruner.recursive_call_sites();
    info.max_call_stack_depth = recursion_pruner.max_call_stack_depth();
    m_recursive_callees = std::move(recursion_pruner.recursive_callees());
    m_speed_excluded_callees = std::move(recursion_pruner.excluded_callees());
  }

  if (m_config.use_call_site_summaries) {
    m_call_site_summarizer = std::make_unique<inliner::CallSiteSummarizer>(
        m_shrinker, callee_caller, caller_callee,
        [this](DexMethod* caller, IRInstruction* insn) -> DexMethod* {
          return this->get_callee(caller, insn);
        },
        [this](DexMethod* callee) -> bool {
          return m_recursive_callees.count(callee) || root(callee) ||
                 m_x_dex_callees.count(callee) || !can_rename(callee) ||
                 m_true_virtual_callees_with_other_call_sites.count(callee) ||
                 m_speed_excluded_callees.count(callee);
        },
        /* filter_fn */ nullptr, &info.call_site_summary_stats);
    m_call_site_summarizer->summarize();
  }

  // Second, compute caller priorities --- the callers get a priority assigned
  // that reflects how many other callers will be waiting for them.
  std::unordered_set<DexMethod*> methods_to_schedule;
  for (auto& p : caller_callee) {
    auto caller = p.first;
    for (auto& q : p.second) {
      auto callee = q.first;
      m_scheduler.add_dependency(const_cast<DexMethod*>(caller), callee);
    }
  }

  // Third, schedule and run tasks for all selected methods.
  if (m_shrinker.enabled() && m_config.shrink_other_methods) {
    walk::code(m_scope, [&](DexMethod* method, IRCode& code) {
      methods_to_schedule.insert(method);
    });
  } else {
    for (auto& p : caller_callee) {
      methods_to_schedule.insert(const_cast<DexMethod*>(p.first));
    }
    for (auto& p : callee_caller) {
      methods_to_schedule.insert(const_cast<DexMethod*>(p.first));
    }
  }

  info.critical_path_length =
      m_scheduler.run(methods_to_schedule.begin(), methods_to_schedule.end());

  m_ab_experiment_context->flush();
  m_ab_experiment_context = nullptr;

  delayed_visibility_changes_apply();
  delayed_invoke_direct_to_static();
  info.waited_seconds = m_scheduler.get_thread_pool().get_waited_seconds();

  if (!need_deconstruct.empty()) {
    workqueue_run<IRCode*>([](IRCode* code) { code->clear_cfg(); },
                           need_deconstruct);
  }
}

DexMethod* MultiMethodInliner::get_callee(DexMethod* caller,
                                          IRInstruction* insn) {
  if (!opcode::is_an_invoke(insn->opcode())) {
    return nullptr;
  }
  auto callee =
      m_concurrent_resolver(insn->get_method(), opcode_to_search(insn));
  auto it = caller_virtual_callee.find(caller);
  if (it == caller_virtual_callee.end()) {
    return callee;
  }
  auto it2 = it->second.find(insn);
  if (it2 == it->second.end()) {
    return callee;
  }
  return it2->second;
}

void MultiMethodInliner::inline_callees(
    DexMethod* caller,
    const std::unordered_set<DexMethod*>& callees,
    bool filter_via_should_inline) {
  TraceContext context{caller};
  std::vector<Inlinable> inlinables;
  {
    auto timer = m_inline_callees_timer.scope();

    // walk the caller opcodes collecting all candidates to inline
    // Build a callee to opcode map
    editable_cfg_adapter::iterate_with_iterator(
        caller->get_code(), [&](const IRList::iterator& it) {
          auto insn = it->insn;
          auto callee = get_callee(caller, insn);
          if (!callee || !callees.count(callee)) {
            return editable_cfg_adapter::LOOP_CONTINUE;
          }
          std::shared_ptr<cfg::ControlFlowGraph> reduced_cfg;
          bool no_return{false};
          size_t insn_size{0};
          if (filter_via_should_inline) {
            auto timer2 = m_inline_callees_should_inline_timer.scope();
            // Cost model is based on fully inlining callee everywhere; let's
            // see if we can get more detailed call-site specific information
            if (should_inline_at_call_site(caller, insn, callee, &no_return,
                                           &reduced_cfg, &insn_size)) {
              always_assert(!no_return);
              // Yes, we know might have dead_blocks and a refined insn_size
            } else if (should_inline_always(callee)) {
              // We'll fully inline the callee without any adjustments
              no_return = false;
              insn_size = get_callee_insn_size(callee);
            } else if (no_return) {
              always_assert(insn_size == 0);
              always_assert(!reduced_cfg);
            } else {
              return editable_cfg_adapter::LOOP_CONTINUE;
            }
          } else {
            insn_size = get_callee_insn_size(callee);
          }
          always_assert(callee->is_concrete());
          if (m_analyze_and_prune_inits && method::is_init(callee) &&
              !no_return) {
            auto timer2 = m_inline_callees_init_timer.scope();
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

          inlinables.push_back((Inlinable){callee, it, insn, no_return,
                                           std::move(reduced_cfg), insn_size});
          return editable_cfg_adapter::LOOP_CONTINUE;
        });
  }
  if (!inlinables.empty()) {
    inline_inlinables(caller, inlinables);
  }
}

void MultiMethodInliner::inline_callees(
    DexMethod* caller,
    const std::unordered_set<IRInstruction*>& insns,
    bool delete_removed_insns) {
  TraceContext context{caller};
  std::vector<Inlinable> inlinables;
  editable_cfg_adapter::iterate_with_iterator(
      caller->get_code(), [&](const IRList::iterator& it) {
        auto insn = it->insn;
        if (insns.count(insn)) {
          auto callee = get_callee(caller, insn);
          if (callee == nullptr) {
            return editable_cfg_adapter::LOOP_CONTINUE;
          }
          always_assert(callee->is_concrete());
          inlinables.push_back((Inlinable){callee, it, insn, false, nullptr,
                                           get_callee_insn_size(callee)});
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });

  inline_inlinables(caller, inlinables, delete_removed_insns);
}

bool MultiMethodInliner::inline_inlinables_need_deconstruct(DexMethod* method) {
  // The mixed CFG, IRCode is used by Switch Inline (only?) where the caller is
  // an IRCode and the callee is a CFG.
  return !method->get_code()->editable_cfg_built();
}

namespace {

// Helper method, as computing inline for a trace could be too expensive.
std::string create_inlining_trace_msg(const DexMethod* caller,
                                      const DexMethod* callee,
                                      IRInstruction* invoke_insn) {
  std::ostringstream oss;
  oss << "inline " << show(callee) << " into " << show(caller) << " ";
  auto features = [&oss](const DexMethod* m, IRInstruction* insn) {
    auto code = m->get_code();
    auto regs = code->cfg_built() ? code->cfg().get_registers_size()
                                  : code->get_registers_size();
    auto opcodes = code->count_opcodes();
    auto blocks = code->cfg_built() ? code->cfg().num_blocks() : (size_t)0;
    auto edges = code->cfg_built() ? code->cfg().num_edges() : (size_t)0;

    oss << regs << "!" << opcodes << "!" << blocks << "!" << edges;

    // Expensive...
    if (code->cfg_built()) {
      loop_impl::LoopInfo info(code->cfg());
      oss << "!" << info.num_loops();
      size_t max_depth{0};
      for (auto* loop : info) {
        max_depth = std::max(max_depth, (size_t)loop->get_loop_depth());
      }
      oss << "!" << max_depth;
      if (insn != nullptr) {
        auto it = code->cfg().find_insn(insn);
        loop_impl::Loop* loop{nullptr};
        if (!it.is_end()) {
          loop = info.get_loop_for(it.block());
        }
        if (loop != nullptr) {
          oss << "!" << loop->get_loop_depth();
        } else {
          oss << "!" << 0;
        }
      } else {
        oss << "!" << 0;
      }
    } else {
      oss << "!0!0!0";
    }
  };
  features(caller, invoke_insn);
  oss << "!";
  features(callee, nullptr);
  return oss.str();
}

} // namespace

void MultiMethodInliner::inline_inlinables(
    DexMethod* caller_method,
    const std::vector<Inlinable>& inlinables,
    bool delete_removed_insns) {
  auto timer = m_inline_inlinables_timer.scope();
  if (for_speed() && m_ab_experiment_context->use_control()) {
    return;
  }

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
      if (!delete_removed_insns) {
        code->cfg().set_removed_insn_ownerhsip(false);
      }
    }
  }

  // attempt to inline all inlinable candidates
  size_t estimated_caller_size = caller->editable_cfg_built()
                                     ? caller->cfg().sum_opcode_sizes()
                                     : caller->sum_opcode_sizes();

  // Prefer inlining smaller methods first, so that we are less likely to hit
  // overall size limit.
  std::vector<Inlinable> ordered_inlinables(inlinables.begin(),
                                            inlinables.end());

  std::stable_sort(ordered_inlinables.begin(),
                   ordered_inlinables.end(),
                   [&](const Inlinable& a, const Inlinable& b) {
                     // First, prefer no-return inlinable, as they cut off
                     // control-flow and thus other inlinables.
                     if (a.no_return != b.no_return) {
                       return a.no_return > b.no_return;
                     }
                     // Second, prefer smaller methods, to avoid hitting size
                     // limits too soon
                     return a.insn_size < b.insn_size;
                   });

  std::vector<DexMethod*> inlined_callees;
  boost::optional<reg_t> cfg_next_caller_reg;
  if (!m_config.unique_inlined_registers) {
    cfg_next_caller_reg = caller->cfg().get_registers_size();
  }
  size_t calls_not_inlinable{0}, calls_not_inlined{0}, no_returns{0},
      unreachable_insns{0}, caller_too_large{0};

  size_t intermediate_shrinkings{0};
  size_t intermediate_remove_unreachable_blocks{0};
  // We only try intermediate remove-unreachable-blocks or shrinking when using
  // the cfg-inliner, as it will invalidate irlist iterators, which are used
  // with the legacy non-cfg-inliner.
  size_t last_intermediate_inlined_callees{0};

  // Once blocks might have been freed, which can happen via
  // remove_unreachable_blocks and shrinking, callsite pointers are no longer
  // valid.
  std::unique_ptr<std::unordered_set<const IRInstruction*>> remaining_callsites;
  auto recompute_remaining_callsites = [caller, &remaining_callsites,
                                        &ordered_inlinables]() {
    if (!remaining_callsites) {
      remaining_callsites =
          std::make_unique<std::unordered_set<const IRInstruction*>>();
      for (const auto& inlinable : ordered_inlinables) {
        remaining_callsites->insert(inlinable.insn);
      }
    }
    std::unordered_set<const IRInstruction*> new_remaining_callsites;
    for (auto& mie : InstructionIterable(caller->cfg())) {
      if (mie.insn->has_method() && remaining_callsites->count(mie.insn)) {
        new_remaining_callsites.insert(mie.insn);
      }
    }
    always_assert(new_remaining_callsites.size() <=
                  remaining_callsites->size());
    *remaining_callsites = std::move(new_remaining_callsites);
  };

  VisibilityChanges visibility_changes;
  std::unordered_set<DexMethod*> visibility_changes_for;
  for (const auto& inlinable : ordered_inlinables) {
    auto callee_method = inlinable.callee;
    auto callee = callee_method->get_code();
    auto callsite_insn = inlinable.insn;

    if (remaining_callsites && !remaining_callsites->count(callsite_insn)) {
      if (!inlinable.no_return) {
        calls_not_inlined++;
      }
      continue;
    }

    if (inlinable.no_return) {
      if (!m_config.throw_after_no_return) {
        continue;
      }
      // we are not actually inlining, but just cutting off control-flow
      // afterwards, inserting an (unreachable) "throw null" instruction
      // sequence.
      auto& caller_cfg = caller->cfg();
      auto callsite_it = caller_cfg.find_insn(callsite_insn);
      if (!callsite_it.is_end()) {
        if (m_config.unique_inlined_registers) {
          cfg_next_caller_reg = caller_cfg.get_registers_size();
        }
        auto temp_reg = *cfg_next_caller_reg;
        if (temp_reg >= caller_cfg.get_registers_size()) {
          caller_cfg.set_registers_size(temp_reg + 1);
        }
        // Copying to avoid cfg limitation
        auto* callsite_copy = new IRInstruction(*callsite_it->insn);
        auto* const_insn = (new IRInstruction(OPCODE_CONST))
                               ->set_dest(temp_reg)
                               ->set_literal(0);
        auto* throw_insn =
            (new IRInstruction(OPCODE_THROW))->set_src(0, temp_reg);
        caller_cfg.replace_insns(callsite_it,
                                 {callsite_copy, const_insn, throw_insn});
        auto p = caller_cfg.remove_unreachable_blocks();
        auto unreachable_insn_count = p.first;
        auto registers_size_possibly_reduced = p.second;
        if (registers_size_possibly_reduced &&
            m_config.unique_inlined_registers) {
          caller_cfg.recompute_registers_size();
          cfg_next_caller_reg = caller_cfg.get_registers_size();
        }
        if (unreachable_insn_count) {
          unreachable_insns += unreachable_insn_count;
          recompute_remaining_callsites();
        }
        estimated_caller_size = caller_cfg.sum_opcode_sizes();
        no_returns++;
      }
      continue;
    }

    if (for_speed()) {
      // This is expensive, but with shrinking/non-cfg inlining prep there's no
      // better way. Needs an explicit check to see whether the instruction has
      // already been shrunk away.
      auto callsite_it = caller->cfg().find_insn(callsite_insn);
      if (!callsite_it.is_end()) {
        auto* block = callsite_it.block();
        if (!m_inline_for_speed->should_inline_callsite(caller_method,
                                                        callee_method, block)) {
          calls_not_inlinable++;
          continue;
        }
      }
    }

    bool caller_too_large_;
    auto not_inlinable = !is_inlinable(caller_method, callee_method,
                                       callsite_insn, estimated_caller_size,
                                       inlinable.insn_size, &caller_too_large_);
    if (not_inlinable && caller_too_large_ &&
        inlined_callees.size() > last_intermediate_inlined_callees) {
      intermediate_remove_unreachable_blocks++;
      last_intermediate_inlined_callees = inlined_callees.size();
      auto p = caller->cfg().remove_unreachable_blocks();
      unreachable_insns += p.first;
      auto registers_size_possibly_reduced = p.second;
      if (registers_size_possibly_reduced &&
          m_config.unique_inlined_registers) {
        caller->cfg().recompute_registers_size();
        cfg_next_caller_reg = caller->cfg().get_registers_size();
      }
      estimated_caller_size = caller->cfg().sum_opcode_sizes();
      recompute_remaining_callsites();
      if (!remaining_callsites->count(callsite_insn)) {
        calls_not_inlined++;
        continue;
      }
      not_inlinable = !is_inlinable(caller_method, callee_method, callsite_insn,
                                    estimated_caller_size, inlinable.insn_size,
                                    &caller_too_large_);
      if (!not_inlinable && m_config.intermediate_shrinking &&
          m_shrinker.enabled()) {
        intermediate_shrinkings++;
        m_shrinker.shrink_method(caller_method);
        cfg_next_caller_reg = caller->cfg().get_registers_size();
        estimated_caller_size = caller->cfg().sum_opcode_sizes();
        recompute_remaining_callsites();
        if (!remaining_callsites->count(callsite_insn)) {
          calls_not_inlined++;
          continue;
        }
        not_inlinable = !is_inlinable(caller_method, callee_method,
                                      callsite_insn, estimated_caller_size,
                                      inlinable.insn_size, &caller_too_large_);
      }
    }
    if (not_inlinable) {
      if (caller_too_large_) {
        caller_too_large++;
      } else {
        calls_not_inlinable++;
      }
      continue;
    }

    TRACE(MMINL, 4, "%s",
          create_inlining_trace_msg(caller_method, callee_method, callsite_insn)
              .c_str());

    if (for_speed()) {
      std::lock_guard<std::mutex> lock(ab_exp_mutex);
      m_ab_experiment_context->try_register_method(caller_method);
    }

    if (m_config.unique_inlined_registers) {
      cfg_next_caller_reg = caller->cfg().get_registers_size();
    }
    auto timer2 = m_inline_with_cfg_timer.scope();
    auto it = m_inlined_invokes_need_cast.find(callsite_insn);
    auto needs_receiver_cast =
        it == m_inlined_invokes_need_cast.end() ? nullptr : it->second;
    bool success = inliner::inline_with_cfg(
        caller_method, callee_method, callsite_insn, needs_receiver_cast,
        *cfg_next_caller_reg, inlinable.reduced_cfg);
    if (!success) {
      calls_not_inlined++;
      continue;
    }
    TRACE(INL, 2, "caller: %s\tcallee: %s",
          caller->cfg_built() ? SHOW(caller->cfg()) : SHOW(caller),
          SHOW(callee));
    estimated_caller_size += inlinable.insn_size;
    if (inlinable.reduced_cfg) {
      visibility_changes.insert(get_visibility_changes(
          *inlinable.reduced_cfg, caller_method->get_class(), callee_method));
    } else {
      visibility_changes_for.insert(callee_method);
    }

    inlined_callees.push_back(callee_method);
  }

  if (!inlined_callees.empty()) {
    for (auto callee_method : visibility_changes_for) {
      visibility_changes.insert(
          get_visibility_changes(callee_method, caller_method->get_class()));
    }
    if (!visibility_changes.empty()) {
      std::lock_guard<std::mutex> guard(m_visibility_changes_mutex);
      if (m_delayed_visibility_changes) {
        m_delayed_visibility_changes->insert(visibility_changes);
      } else {
        visibility_changes_apply_and_record_make_static(visibility_changes);
      }
    }
    m_inlined.insert(inlined_callees.begin(), inlined_callees.end());
  }

  for (IRCode* code : need_deconstruct) {
    code->clear_cfg();
  }

  info.calls_inlined += inlined_callees.size();
  if (calls_not_inlinable) {
    info.calls_not_inlinable += calls_not_inlinable;
  }
  if (calls_not_inlined) {
    info.calls_not_inlined += calls_not_inlined;
  }
  if (no_returns) {
    info.no_returns += no_returns;
  }
  if (unreachable_insns) {
    info.unreachable_insns += unreachable_insns;
  }
  if (intermediate_shrinkings) {
    info.intermediate_shrinkings += intermediate_shrinkings;
  }
  if (intermediate_remove_unreachable_blocks) {
    info.intermediate_remove_unreachable_blocks +=
        intermediate_remove_unreachable_blocks;
  }
  if (caller_too_large) {
    info.caller_too_large += caller_too_large;
  }
}

void MultiMethodInliner::postprocess_method(DexMethod* method) {
  TraceContext context(method);
  if (m_shrinker.enabled() && !method->rstate.no_optimizations()) {
    m_shrinker.shrink_method(method);
  }

  bool is_callee = !!callee_caller.count(method);
  if (!is_callee) {
    // This method isn't the callee of another caller, so we can stop here.
    return;
  }

  compute_callee_costs(method);
}

void MultiMethodInliner::compute_callee_costs(DexMethod* method) {
  auto fully_inlined_cost = get_fully_inlined_cost(method);
  always_assert(fully_inlined_cost);

  auto callee_call_site_invokes =
      m_call_site_summarizer
          ? m_call_site_summarizer->get_callee_call_site_invokes(method)
          : nullptr;
  if (callee_call_site_invokes != nullptr) {
    std::unordered_map<const CallSiteSummary*,
                       std::vector<const IRInstruction*>>
        invokes;
    for (auto invoke_insn : *callee_call_site_invokes) {
      const auto* call_site_summary =
          m_call_site_summarizer->get_instruction_call_site_summary(
              invoke_insn);
      always_assert(call_site_summary != nullptr);
      invokes[call_site_summary].push_back(invoke_insn);
    }
    for (auto& p : invokes) {
      m_scheduler.augment(method, [this, call_site_summary = p.first,
                                   insns = p.second, method]() {
        TraceContext context(method);
        // Populate caches
        auto timer = m_call_site_inlined_cost_timer.scope();
        bool keep_reduced_cfg = false;
        for (auto insn : insns) {
          if (should_inline_at_call_site(nullptr, insn, method)) {
            keep_reduced_cfg = true;
          }
        }
        if (!keep_reduced_cfg) {
          CalleeCallSiteSummary key{method, call_site_summary};
          auto inlined_cost = m_call_site_inlined_costs.get(key, nullptr);
          if (inlined_cost) {
            inlined_cost->reduced_cfg.reset();
          }
        }
      });
    }
  }

  m_scheduler.augment(method, [this, method]() {
    // Populate caches
    get_callee_insn_size(method);
    get_callee_type_refs(method);
    if (m_ref_checkers) {
      get_callee_code_refs(method);
    }
  });

  // The should_inline_always caching depends on all other caches, so we augment
  // it as a "continuation".
  m_scheduler.augment(
      method,
      [this, method] {
        TraceContext context(method);
        // Populate cache
        should_inline_always(method);
      },
      /* continuation */ true);
}

/**
 * Defines the set of rules that determine whether a function is inlinable.
 */
bool MultiMethodInliner::is_inlinable(const DexMethod* caller,
                                      const DexMethod* callee,
                                      const IRInstruction* insn,
                                      uint64_t estimated_caller_size,
                                      uint64_t estimated_callee_size,
                                      bool* caller_too_large_) {
  TraceContext context{caller};
  if (caller_too_large_) {
    *caller_too_large_ = false;
  }
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
  if (cannot_inline_opcodes(caller, callee, insn)) {
    return false;
  }
  if (!callee->rstate.force_inline()) {
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
      info.api_level_mismatch++;
      return false;
    }

    if (callee->rstate.dont_inline()) {
      if (insn) {
        log_nopt(INL_DO_NOT_INLINE, caller, insn);
      }
      return false;
    }

    if (caller_too_large(caller->get_class(), estimated_caller_size,
                         estimated_callee_size)) {
      if (insn) {
        log_nopt(INL_TOO_BIG, caller, insn);
      }
      if (caller_too_large_) {
        *caller_too_large_ = true;
      }
      return false;
    }

    if (caller->get_class() != callee->get_class() && m_ref_checkers &&
        problematic_refs(caller, callee)) {
      return false;
    }
  }

  return true;
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
                                              uint64_t estimated_callee_size,
                                              uint64_t max) {
  // INSTRUCTION_BUFFER is added because the final method size is often larger
  // than our estimate -- during the sync phase, we may have to pick larger
  // branch opcodes to encode large jumps.
  if (estimated_caller_size + estimated_callee_size >
      max - std::min(m_config.instruction_size_buffer, max)) {
    return true;
  }
  return false;
}

bool MultiMethodInliner::caller_too_large(DexType* caller_type,
                                          uint64_t estimated_caller_size,
                                          uint64_t estimated_callee_size) {
  if (is_estimate_over_max(estimated_caller_size, estimated_callee_size,
                           HARD_MAX_INSTRUCTION_SIZE)) {
    return true;
  }

  if (!m_config.enforce_method_size_limit) {
    return false;
  }

  if (m_config.allowlist_no_method_limit.count(caller_type)) {
    return false;
  }

  if (is_estimate_over_max(estimated_caller_size, estimated_callee_size,
                           m_config.soft_max_instruction_size)) {
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

  // non-root methods that are only ever called once should always be inlined,
  // as the method can be removed afterwards
  const auto& callers = callee_caller.at(callee);
  if (callers.size() == 1 && callers.begin()->second == 1 && !root(callee) &&
      !m_recursive_callees.count(callee) && !m_x_dex_callees.count(callee) &&
      !m_true_virtual_callees_with_other_call_sites.count(callee)) {
    return true;
  }

  return false;
}

bool MultiMethodInliner::should_inline_always(const DexMethod* callee) {
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

static float get_invoke_cost(const DexMethod* callee, float result_used) {
  float invoke_cost = COST_INVOKE + result_used * COST_MOVE_RESULT;
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
               insn->has_string()) {
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
  TRACE(INLINE, 5, "  %zu: %s", cost, SHOW(insn));
  return cost;
}

/*
 * Try to estimate number of code units (2 bytes each) overhead (instructions,
 * metadata) that exists for this block; this doesn't include the cost of
 * the instructions in the block, which are accounted for elsewhere.
 */
static size_t get_inlined_cost(const std::vector<cfg::Block*>& blocks,
                               size_t index,
                               const std::vector<cfg::Edge*>& succs) {
  auto block = blocks.at(index);
  switch (block->branchingness()) {
  case opcode::Branchingness::BRANCH_GOTO:
  case opcode::Branchingness::BRANCH_IF:
  case opcode::Branchingness::BRANCH_SWITCH: {
    if (succs.empty()) {
      return 0;
    }
    if (succs.size() > 2) {
      // a switch
      return 4 + 3 * succs.size();
    }
    // a (possibly conditional) branch; each feasible non-fallthrough edge has a
    // cost
    size_t cost{0};
    auto next_block =
        index == blocks.size() - 1 ? nullptr : blocks.at(index + 1);
    for (auto succ : succs) {
      always_assert(succ->target() != nullptr);
      if (next_block != succ->target()) {
        // we have a non-fallthrough edge
        cost++;
      }
    }
    return cost;
  }
  default:
    return 0;
  }
}

std::shared_ptr<cfg::ControlFlowGraph>
MultiMethodInliner::apply_call_site_summary(
    bool is_static,
    DexType* declaring_type,
    DexProto* proto,
    const cfg::ControlFlowGraph& original_cfg,
    const CallSiteSummary* call_site_summary) {
  if (!call_site_summary) {
    return nullptr;
  }

  if (call_site_summary->arguments.is_top()) {
    if (proto->is_void() || call_site_summary->result_used) {
      return nullptr;
    }
  }

  // Clone original cfg
  IRCode cloned_code(std::make_unique<cfg::ControlFlowGraph>());
  original_cfg.deep_copy(&cloned_code.cfg());

  // If result is not used, change all return-* instructions to return-void (and
  // let local-dce remove the code that leads to it).
  if (!proto->is_void() && !call_site_summary->result_used) {
    proto = DexProto::make_proto(type::_void(), proto->get_args());
    for (auto& mie : InstructionIterable(cloned_code.cfg())) {
      if (opcode::is_a_return(mie.insn->opcode())) {
        mie.insn->set_opcode(OPCODE_RETURN_VOID);
        mie.insn->set_srcs_size(0);
      }
    }
  }

  // Run constant-propagation with call-site specific arguments, and then run
  // local-dce
  ConstantEnvironment initial_env =
      constant_propagation::interprocedural::env_with_params(
          is_static, &cloned_code, call_site_summary->arguments);
  constant_propagation::Transform::Config config;
  // No need to add extra instructions to load constant params, we'll pass those
  // in anyway
  config.add_param_const = false;
  m_shrinker.constant_propagation(is_static, declaring_type, proto,
                                  &cloned_code, initial_env, config);
  m_shrinker.local_dce(&cloned_code, /* normalize_new_instances */ false);

  // Re-build cfg once more to get linearized representation, good for
  // predicting fallthrough branches
  cloned_code.build_cfg(/* editable */ true);

  // And a final clone to move the cfg into a long-lived shared-ptr.
  auto res = std::make_shared<cfg::ControlFlowGraph>();
  cloned_code.cfg().deep_copy(res.get());
  res->set_insn_ownership(true);

  cloned_code.cfg().set_insn_ownership(true);
  // Note that we are not clearing the cloned_code.cfg(). It will get destroyed
  // now, and delete all instruction copies that it now owns.

  return res;
}

InlinedCost MultiMethodInliner::get_inlined_cost(
    bool is_static,
    DexType* declaring_type,
    DexProto* proto,
    const IRCode* code,
    const CallSiteSummary* call_site_summary) {
  size_t cost{0};
  std::shared_ptr<cfg::ControlFlowGraph> reduced_cfg;
  size_t returns{0};
  std::unordered_set<DexMethodRef*> method_refs_set;
  std::unordered_set<const void*> other_refs_set;
  auto analyze_refs = [&](IRInstruction* insn) {
    if (insn->has_method()) {
      auto cls = type_class(insn->get_method()->get_class());
      if (cls && !cls->is_external()) {
        method_refs_set.insert(insn->get_method());
      }
    }
    if (insn->has_field()) {
      auto cls = type_class(insn->get_field()->get_class());
      if (cls && !cls->is_external()) {
        other_refs_set.insert(insn->get_field());
      }
    }
    if (insn->has_type()) {
      auto type = type::get_element_type_if_array(insn->get_type());
      auto cls = type_class(type);
      if (cls && !cls->is_external()) {
        other_refs_set.insert(type);
      }
    }
  };
  size_t insn_size;
  if (code->editable_cfg_built()) {
    reduced_cfg = apply_call_site_summary(is_static, declaring_type, proto,
                                          code->cfg(), call_site_summary);
    auto cfg = reduced_cfg ? reduced_cfg.get() : &code->cfg();
    auto blocks = cfg->blocks();
    for (size_t i = 0; i < blocks.size(); ++i) {
      auto block = blocks.at(i);
      for (auto& mie : InstructionIterable(block)) {
        auto insn = mie.insn;
        cost += ::get_inlined_cost(insn);
        analyze_refs(insn);
      }
      cost += ::get_inlined_cost(blocks, i, block->succs());
      if (block->branchingness() == opcode::Branchingness::BRANCH_RETURN) {
        returns++;
      }
    }
    insn_size = cfg->sum_opcode_sizes();
  } else {
    editable_cfg_adapter::iterate(code, [&](const MethodItemEntry& mie) {
      auto insn = mie.insn;
      cost += ::get_inlined_cost(insn);
      if (opcode::is_a_return(insn->opcode())) {
        returns++;
      }
      analyze_refs(insn);
      return editable_cfg_adapter::LOOP_CONTINUE;
    });
    insn_size = code->sum_opcode_sizes();
  }
  if (returns > 1) {
    // if there's more than one return, gotos will get introduced to merge
    // control flow
    cost += returns - 1;
  }

  auto result_used =
      call_site_summary ? call_site_summary->result_used : !proto->is_void();

  return (InlinedCost){cost,
                       (float)cost,
                       (float)method_refs_set.size(),
                       (float)other_refs_set.size(),
                       !returns,
                       (float)result_used,
                       std::move(reduced_cfg),
                       insn_size};
}

const InlinedCost* MultiMethodInliner::get_fully_inlined_cost(
    const DexMethod* callee) {
  auto inlined_cost = m_fully_inlined_costs.get(callee, nullptr);
  if (inlined_cost) {
    return inlined_cost.get();
  }
  inlined_cost = std::make_shared<InlinedCost>(
      get_inlined_cost(is_static(callee), callee->get_class(),
                       callee->get_proto(), callee->get_code()));
  TRACE(INLINE, 4, "get_fully_inlined_cost(%s) = {%zu,%f,%f,%f,%s,%f,%d,%zu}",
        SHOW(callee), inlined_cost->full_code, inlined_cost->code,
        inlined_cost->method_refs, inlined_cost->other_refs,
        inlined_cost->no_return ? "no_return" : "return",
        inlined_cost->result_used, !!inlined_cost->reduced_cfg,
        inlined_cost->insn_size);
  m_fully_inlined_costs.update(
      callee,
      [&](const DexMethod*, std::shared_ptr<InlinedCost>& value, bool exists) {
        if (exists) {
          // We wasted some work, and some other thread beat us. Oh well...
          always_assert(*value == *inlined_cost);
          inlined_cost = value;
          return;
        }
        value = inlined_cost;
      });

  return inlined_cost.get();
}

const InlinedCost* MultiMethodInliner::get_call_site_inlined_cost(
    const IRInstruction* invoke_insn, const DexMethod* callee) {
  auto res = m_invoke_call_site_inlined_costs.get(invoke_insn, boost::none);
  if (res) {
    return *res;
  }

  auto call_site_summary =
      m_call_site_summarizer
          ? m_call_site_summarizer->get_instruction_call_site_summary(
                invoke_insn)
          : nullptr;
  if (call_site_summary == nullptr) {
    res = nullptr;
  } else {
    res = get_call_site_inlined_cost(call_site_summary, callee);
  }

  always_assert(res);
  m_invoke_call_site_inlined_costs.emplace(invoke_insn, res);
  return *res;
}

const InlinedCost* MultiMethodInliner::get_call_site_inlined_cost(
    const CallSiteSummary* call_site_summary, const DexMethod* callee) {
  auto fully_inlined_cost = get_fully_inlined_cost(callee);
  always_assert(fully_inlined_cost);
  if (fully_inlined_cost->full_code > MAX_COST_FOR_CONSTANT_PROPAGATION) {
    return nullptr;
  }

  CalleeCallSiteSummary key{callee, call_site_summary};
  auto inlined_cost = m_call_site_inlined_costs.get(key, nullptr);
  if (inlined_cost) {
    return inlined_cost.get();
  }

  inlined_cost = std::make_shared<InlinedCost>(get_inlined_cost(
      is_static(callee), callee->get_class(), callee->get_proto(),
      callee->get_code(), call_site_summary));
  TRACE(INLINE, 4,
        "get_call_site_inlined_cost(%s) = {%zu,%f,%f,%f,%s,%f,%d,%zu}",
        call_site_summary->get_key().c_str(), inlined_cost->full_code,
        inlined_cost->code, inlined_cost->method_refs, inlined_cost->other_refs,
        inlined_cost->no_return ? "no_return" : "return",
        inlined_cost->result_used, !!inlined_cost->reduced_cfg,
        inlined_cost->insn_size);
  if (inlined_cost->insn_size >= fully_inlined_cost->insn_size) {
    inlined_cost->reduced_cfg.reset();
  }
  m_call_site_inlined_costs.update(key,
                                   [&](const CalleeCallSiteSummary&,
                                       std::shared_ptr<InlinedCost>& value,
                                       bool exists) {
                                     if (exists) {
                                       // We wasted some work, and some other
                                       // thread beat us. Oh well...
                                       always_assert(*value == *inlined_cost);
                                       inlined_cost = value;
                                       return;
                                     }
                                     value = inlined_cost;
                                   });

  return inlined_cost.get();
}

const InlinedCost* MultiMethodInliner::get_average_inlined_cost(
    const DexMethod* callee) {
  auto inlined_cost = m_average_inlined_costs.get(callee, nullptr);
  if (inlined_cost) {
    return inlined_cost.get();
  }

  auto fully_inlined_cost = get_fully_inlined_cost(callee);
  always_assert(fully_inlined_cost);

  size_t callees_analyzed{0};
  size_t callees_unused_results{0};
  size_t callees_no_return{0};

  const std::vector<CallSiteSummaryOccurrences>*
      callee_call_site_summary_occurrences;
  if (fully_inlined_cost->full_code > MAX_COST_FOR_CONSTANT_PROPAGATION ||
      !(callee_call_site_summary_occurrences =
            m_call_site_summarizer
                ? m_call_site_summarizer
                      ->get_callee_call_site_summary_occurrences(callee)
                : nullptr)) {
    inlined_cost = std::make_shared<InlinedCost>(*fully_inlined_cost);
  } else {
    inlined_cost = std::make_shared<InlinedCost>((InlinedCost){
        fully_inlined_cost->full_code, 0.0f, 0.0f, 0.0f, true, 0.0f, {}, 0});
    bool callee_has_result = !callee->get_proto()->is_void();
    for (auto& p : *callee_call_site_summary_occurrences) {
      const auto call_site_summary = p.first;
      const auto count = p.second;
      auto call_site_inlined_cost =
          get_call_site_inlined_cost(call_site_summary, callee);
      always_assert(call_site_inlined_cost);
      if (callee_has_result && !call_site_summary->result_used) {
        callees_unused_results += count;
      }
      inlined_cost->code += call_site_inlined_cost->code * count;
      inlined_cost->method_refs += call_site_inlined_cost->method_refs * count;
      inlined_cost->other_refs += call_site_inlined_cost->other_refs * count;
      inlined_cost->result_used += call_site_inlined_cost->result_used * count;
      if (call_site_inlined_cost->no_return) {
        callees_no_return++;
      } else {
        inlined_cost->no_return = false;
      }
      if (call_site_inlined_cost->insn_size > inlined_cost->insn_size) {
        inlined_cost->insn_size = call_site_inlined_cost->insn_size;
      }
      callees_analyzed += count;
    };

    always_assert(callees_analyzed > 0);
    // compute average costs
    inlined_cost->code /= callees_analyzed;
    inlined_cost->method_refs /= callees_analyzed;
    inlined_cost->other_refs /= callees_analyzed;
    inlined_cost->result_used /= callees_analyzed;
  }
  TRACE(INLINE, 4, "get_average_inlined_cost(%s) = {%zu,%f,%f,%f,%s,%f,%zu}",
        SHOW(callee), inlined_cost->full_code, inlined_cost->code,
        inlined_cost->method_refs, inlined_cost->other_refs,
        inlined_cost->no_return ? "no_return" : "return",
        inlined_cost->result_used, inlined_cost->insn_size);
  m_average_inlined_costs.update(
      callee,
      [&](const DexMethod*, std::shared_ptr<InlinedCost>& value, bool exists) {
        if (exists) {
          // We wasted some work, and some other thread beat us. Oh well...
          always_assert(*value == *inlined_cost);
          inlined_cost = value;
          return;
        }
        value = inlined_cost;
        if (callees_analyzed == 0) {
          return;
        }
        info.constant_invoke_callees_analyzed += callees_analyzed;
        info.constant_invoke_callees_unused_results += callees_unused_results;
        info.constant_invoke_callees_no_return += callees_no_return;
      });
  return inlined_cost.get();
}

bool MultiMethodInliner::can_inline_init(const DexMethod* init_method) {
  auto opt_can_inline_init = m_can_inline_init.get(init_method, boost::none);
  if (opt_can_inline_init) {
    return *opt_can_inline_init;
  }

  const auto* finalizable_fields = m_shrinker.get_finalizable_fields();
  bool res =
      constructor_analysis::can_inline_init(init_method, finalizable_fields);
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
  if (root(callee) || m_recursive_callees.count(callee) ||
      m_x_dex_callees.count(callee) ||
      m_true_virtual_callees_with_other_call_sites.count(callee) ||
      !m_config.multiple_callers) {
    return true;
  }

  const auto& callers = callee_caller.at(callee);

  // Can we inline the init-callee into all callers?
  // If not, then we can give up, as there's no point in making the case that
  // we can eliminate the callee method based on pervasive inlining.
  if (m_analyze_and_prune_inits && method::is_init(callee)) {
    if (!callee->get_code()->editable_cfg_built()) {
      return true;
    }
    if (!can_inline_init(callee)) {
      for (auto& p : callers) {
        auto caller = p.first;
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

  // 1. Determine costs of inlining

  auto inlined_cost = get_average_inlined_cost(callee);
  always_assert(inlined_cost);

  boost::optional<CalleeCallerRefs> callee_caller_refs;
  float cross_dex_penalty{0};
  if (m_mode != IntraDex && !is_private(callee)) {
    callee_caller_refs = get_callee_caller_refs(callee);
    if (callee_caller_refs->same_class) {
      callee_caller_refs = boost::none;
    } else {
      // Inlining methods into different classes might lead to worse
      // cross-dex-ref minimization results.
      cross_dex_penalty = inlined_cost->method_refs;
      if (callee_caller_refs->classes > 1 &&
          (inlined_cost->method_refs + inlined_cost->other_refs) > 0) {
        cross_dex_penalty++;
      }
    }
  }

  // 2. Determine costs of keeping the invoke instruction

  size_t caller_count{0};
  for (auto& p : callers) {
    caller_count += p.second;
  }
  float invoke_cost = get_invoke_cost(callee, inlined_cost->result_used);
  TRACE(INLINE, 3,
        "[too_many_callers] %zu calls to %s; cost: inlined %f + %f, invoke %f",
        caller_count, SHOW(callee), inlined_cost->code, cross_dex_penalty,
        invoke_cost);

  size_t classes = callee_caller_refs ? callee_caller_refs->classes : 0;

  // The cost of keeping a method amounts of somewhat fixed metadata overhead,
  // plus the method body, which we approximate with the inlined cost.
  size_t method_cost = COST_METHOD + inlined_cost->full_code;
  auto methods_cost = method_cost;

  // If we inline invocations to this method everywhere, we could delete the
  // method. Is this worth it, given the number of callsites and costs
  // involved?
  if (inlined_cost->code * caller_count + classes * cross_dex_penalty >
      invoke_cost * caller_count + methods_cost) {
    return true;
  }

  // We can't eliminate the method entirely if it's not inlinable
  for (auto& p : callers) {
    auto caller = p.first;
    // We can't account here in detail for the caller and callee size. We hope
    // for the best, and assume that the caller is empty, and we'll use the
    // (maximum) insn_size for all inlined-costs. We'll check later again at
    // each individual call-site whether the size limits hold.
    if (!is_inlinable(caller, callee, /* insn */ nullptr,
                      /* estimated_caller_size */ 0,
                      /* estimated_callee_size */ inlined_cost->insn_size)) {
      return true;
    }
  }

  return false;
}

bool MultiMethodInliner::should_inline_at_call_site(
    DexMethod* caller,
    const IRInstruction* invoke_insn,
    DexMethod* callee,
    bool* no_return,
    std::shared_ptr<cfg::ControlFlowGraph>* reduced_cfg,
    size_t* insn_size) {
  auto inlined_cost = get_call_site_inlined_cost(invoke_insn, callee);
  if (!inlined_cost) {
    return false;
  }

  float cross_dex_penalty{0};
  if (m_mode != IntraDex && !is_private(callee) &&
      (caller == nullptr || caller->get_class() != callee->get_class())) {
    // Inlining methods into different classes might lead to worse
    // cross-dex-ref minimization results.
    cross_dex_penalty = inlined_cost->method_refs;
    if (inlined_cost->method_refs + inlined_cost->other_refs > 0) {
      cross_dex_penalty++;
    }
  }

  float invoke_cost = get_invoke_cost(callee, inlined_cost->result_used);
  if (inlined_cost->code + cross_dex_penalty > invoke_cost) {
    if (no_return) {
      *no_return = inlined_cost->no_return;
    }
    return false;
  }

  if (reduced_cfg) {
    *reduced_cfg = inlined_cost->reduced_cfg;
  }
  if (insn_size) {
    *insn_size = inlined_cost->insn_size;
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
  std::vector<DexType*> types;
  callee->get_code()->gather_catch_types(types);
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
bool MultiMethodInliner::cannot_inline_opcodes(const DexMethod* caller,
                                               const DexMethod* callee,
                                               const IRInstruction* invk_insn) {
  bool can_inline = true;
  editable_cfg_adapter::iterate(
      callee->get_code(), [&](const MethodItemEntry& mie) {
        auto insn = mie.insn;
        if (create_vmethod(insn, callee, caller)) {
          if (invk_insn) {
            log_nopt(INL_CREATE_VMETH, caller, invk_insn);
          }
          can_inline = false;
          return editable_cfg_adapter::LOOP_BREAK;
        }
        if (outlined_invoke_outlined(insn, caller)) {
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
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
  if (!can_inline) {
    return true;
  }
  if (m_config.respect_sketchy_methods) {
    auto timer = m_cannot_inline_sketchy_code_timer.scope();
    if (monitor_count::cannot_inline_sketchy_code(
            *caller->get_code(), *callee->get_code(), invk_insn)) {
      return true;
    }
  }
  return false;
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
                                        const DexMethod* caller) {
  auto opcode = insn->opcode();
  if (opcode == OPCODE_INVOKE_DIRECT) {
    auto method =
        m_concurrent_resolver(insn->get_method(), MethodSearch::Direct);
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
    if (!can_rename(method)) {
      info.need_vmethod++;
      return true;
    }
  }
  return false;
}

bool MultiMethodInliner::outlined_invoke_outlined(IRInstruction* insn,
                                                  const DexMethod* caller) {
  if (!PositionPatternSwitchManager::
          CAN_OUTLINED_METHOD_INVOKE_OUTLINED_METHOD &&
      insn->opcode() == OPCODE_INVOKE_STATIC && is_outlined_method(caller) &&
      is_outlined_method(insn->get_method())) {
    // TODO: Remove this limitation imposed by symbolication infrastructure.
    return true;
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
    auto res_method = m_concurrent_resolver(method, MethodSearch::Virtual);
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

std::shared_ptr<CodeRefs> MultiMethodInliner::get_callee_code_refs(
    const DexMethod* callee) {
  if (m_callee_code_refs) {
    auto cached = m_callee_code_refs->get(callee, nullptr);
    if (cached) {
      return cached;
    }
  }

  auto code_refs = std::make_shared<CodeRefs>(callee);
  if (m_callee_code_refs) {
    m_callee_code_refs->emplace(callee, code_refs);
  }
  return code_refs;
}

std::shared_ptr<std::vector<DexType*>> MultiMethodInliner::get_callee_type_refs(
    const DexMethod* callee) {
  if (m_callee_type_refs) {
    auto cached = m_callee_type_refs->get(callee, nullptr);
    if (cached) {
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

  auto type_refs = std::make_shared<std::vector<DexType*>>();
  for (auto type : type_refs_set) {
    // filter out what xstores.illegal_ref(...) doesn't care about
    if (type_class_internal(type) == nullptr) {
      continue;
    }
    type_refs->push_back(type);
  }

  if (m_callee_type_refs) {
    m_callee_type_refs->emplace(callee, type_refs);
  }
  return type_refs;
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
  for (auto& p : callers) {
    auto caller = p.first;
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

bool MultiMethodInliner::problematic_refs(const DexMethod* caller,
                                          const DexMethod* callee) {
  always_assert(m_ref_checkers);
  auto callee_code_refs = get_callee_code_refs(callee);
  always_assert(callee_code_refs);
  const auto& xstores = m_shrinker.get_xstores();
  size_t store_idx = xstores.get_store_idx(caller->get_class());
  auto& ref_checker = m_ref_checkers->at(store_idx);
  if (!ref_checker->check_code_refs(*callee_code_refs)) {
    info.problematic_refs++;
    return true;
  }
  return false;
}

bool MultiMethodInliner::cross_store_reference(const DexMethod* caller,
                                               const DexMethod* callee) {
  auto callee_type_refs = get_callee_type_refs(callee);
  always_assert(callee_type_refs);
  const auto& xstores = m_shrinker.get_xstores();
  size_t store_idx = xstores.get_store_idx(caller->get_class());
  for (auto type : *callee_type_refs) {
    if (xstores.illegal_ref(store_idx, type)) {
      info.cross_store++;
      return true;
    }
  }

  return false;
}

void MultiMethodInliner::delayed_visibility_changes_apply() {
  visibility_changes_apply_and_record_make_static(
      *m_delayed_visibility_changes);
}

void MultiMethodInliner::visibility_changes_apply_and_record_make_static(
    const VisibilityChanges& visibility_changes) {
  visibility_changes.apply();
  // any method that was just made public and isn't virtual or a constructor or
  // static must be made static
  for (auto method : visibility_changes.methods) {
    always_assert(is_public(method));
    if (!method->is_virtual() && !method::is_init(method) &&
        !is_static(method)) {
      always_assert(can_rename(method));
      always_assert(method->is_concrete());
      m_delayed_make_static.insert(method);
    }
  }
}

void MultiMethodInliner::delayed_invoke_direct_to_static() {
  if (m_delayed_make_static.empty()) {
    return;
  }
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
  walk::parallel::opcodes(
      m_scope, [](DexMethod* meth) { return true; },
      [&](DexMethod*, IRInstruction* insn) {
        auto op = insn->opcode();
        if (op == OPCODE_INVOKE_DIRECT) {
          auto m = insn->get_method()->as_def();
          if (m && m_delayed_make_static.count_unsafe(m)) {
            insn->set_opcode(OPCODE_INVOKE_STATIC);
          }
        }
      });
  m_delayed_make_static.clear();
}

namespace inliner {

// return true on successful inlining, false otherwise
bool inline_with_cfg(
    DexMethod* caller_method,
    DexMethod* callee_method,
    IRInstruction* callsite,
    DexType* needs_receiver_cast,
    size_t next_caller_reg,
    const std::shared_ptr<cfg::ControlFlowGraph>& reduced_cfg) {

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

  auto callee_code = callee_method->get_code();
  always_assert(callee_code->editable_cfg_built());
  auto& callee_cfg = reduced_cfg ? *reduced_cfg : callee_code->cfg();

  bool is_trivial = true;
  for (auto& mie : InstructionIterable(callee_cfg)) {
    if (mie.insn->opcode() != OPCODE_RETURN_VOID &&
        !opcode::is_load_param(mie.insn->opcode())) {
      is_trivial = false;
      break;
    }
  }
  if (is_trivial) {
    // no need to go through expensive general inlining, which would also add
    // unnecessary or even dubious positions
    caller_cfg.remove_insn(callsite_it);
    return true;
  }

  if (caller_code->get_debug_item() == nullptr) {
    // Create an empty item so that debug info of inlinee does not get lost.
    caller_code->set_debug_item(std::make_unique<DexDebugItem>());
    // Create a fake position.
    caller_cfg.insert_before(
        caller_cfg.entry_block(),
        caller_cfg.entry_block()->get_first_non_param_loading_insn(),
        DexPosition::make_synthetic_entry_position(caller_method));
  }

  // Logging before the call to inline_cfg to get the most relevant line
  // number near callsite before callsite gets replaced. Should be ok as
  // inline_cfg does not fail to inline.
  log_opt(INLINED, caller_method, callsite);

  cfg::CFGInliner::inline_cfg(&caller_cfg, callsite_it, needs_receiver_cast,
                              callee_cfg, next_caller_reg);

  return true;
}

} // namespace inliner
