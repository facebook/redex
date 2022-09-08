/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This pass identifies tracable object allocations that don't escape, and then
 * attempts to inline all code interacting with the local object, turning all
 * instance fields into registers. The changes are only applied when the
 * estimated savings are not negative. This helps reduce...
 * - object allocations at runtime, and
 * - code size by eliminating a many of the involved classes, fields and
 *   methods.
 *
 * At the core is an interprocedural escape analysis with method-level summaries
 * that...
 * - may include results of method invocations as allocations, and
 * - follows arguments to non-true-virtual method invocations.
 *
 * This pass is conservative: Any use of an object allocation that isn't fully
 * understood, e.g. an external method invocation, causes that allocation to
 * become ineligable for optimization. In any case, this pass will not transform
 * a root method with the no_optimizations annotation.
 *
 * The pass computes...
 * - method summaries, indicating whether a method allocates and returns an
 *   object that doesn't otherwise escape, and which method arguments don't
 *   escape
 * - "inline anchors", which are particular instructions (in particular methods)
 *   which produce a new unescaped object, either by directly allocating it or
 *   invoking a method that directly or indirect allocates and returns an object
 *   that doesn't otherwise escape, and then possibly use that object in ways
 *   where it doesn't escape
 * - "root methods", which are all the methods which contain "inline anchors" of
 *   types whose allocation instructions are all ultimately inlinably anchored.
 * - "reduced methods", which are root methods where all inlinable anchors got
 *   fully inlined, and the fields of allocated objects got turned into
 *   registers (and the transformation does not produce estimated negative net
 *   savings)
 *
 * Notes:
 * - The transformation doesn't directly eliminate the object allocation, as the
 *   object might be involved in some identity comparisons, e.g. for
 *   null-checks. Instead, the object allocation gets rewritten to create an
 *   object of type java.lang.Object, and other optimizations such as
 *   constant-propagation and local-dead-code-elimination should be able to
 *   remove that remaining code in most cases.
 *
 * Ideas for future work:
 * - Support check-cast instructions for singleton-allocations
 * - Support conditional branches over either zero or single allocations
 * - Refine the tracing of object allocations in root methods to ignore
 *   unanchored object allocations
 * - Instead of inlining all invoked methods, consider transforming those which
 *   do not mutate or compare the allocated object as follows: instead of
 *   passing in the allocated object via an argument, pass in all read fields
 *   are passed in as separate arguments. This could reduce the size increase
 *   due to multiple inlined method body copies. We already do something similar
 *   for constructors.
 */

#include "ObjectEscapeAnalysis.h"

#include <algorithm>
#include <optional>

#include "ApiLevelChecker.h"
#include "BaseIRAnalyzer.h"
#include "CFGMutation.h"
#include "ConfigFiles.h"
#include "ConstantAbstractDomain.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "ExpandableMethodParams.h"
#include "HierarchyUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "Inliner.h"
#include "Lazy.h"
#include "LiveRange.h"
#include "MethodOverrideGraph.h"
#include "PassManager.h"
#include "PatriciaTreeMap.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "PatriciaTreeSet.h"
#include "PatriciaTreeSetAbstractDomain.h"
#include "Resolver.h"
#include "Show.h"
#include "StringBuilder.h"
#include "Walkers.h"

using namespace sparta;
namespace mog = method_override_graph;
namespace hier = hierarchy_util;

namespace {

// How deep callee chains will be considered.
constexpr int MAX_INLINE_INVOKES_ITERATIONS = 8;

constexpr size_t MAX_INLINE_SIZE = 48;

// Don't even try to inline an incompletely inlinable type if a very rough
// estimate predicts an increase exceeding this threshold in code units.
constexpr float INCOMPLETE_ESTIMATED_DELTA_THRESHOLD = 0;

// Overhead of having a method and its metadata.
constexpr size_t COST_METHOD = 16;

// Overhead of having a class and its metadata.
constexpr size_t COST_CLASS = 48;

// Overhead of having a field and its metadata.
constexpr size_t COST_FIELD = 8;

// Typical overhead of calling a method, without move-result overhead.
constexpr float COST_INVOKE = 4.7f;

// Typical overhead of having move-result instruction.
constexpr float COST_MOVE_RESULT = 3.0f;

// Overhead of a new-instance instruction.
constexpr float COST_NEW_INSTANCE = 2.0f;

using Locations = std::vector<std::pair<DexMethod*, const IRInstruction*>>;

// Collect all allocation and invoke instructions, as well as non-virtual
// invocation dependencies.
void analyze_scope(
    const Scope& scope,
    const std::unordered_set<const DexMethod*>& non_overridden_virtuals,
    std::unordered_map<DexType*, Locations>* new_instances,
    std::unordered_map<DexMethod*, Locations>* invokes,
    std::unordered_map<DexMethod*, std::unordered_set<DexMethod*>>*
        dependencies) {
  Timer t("analyze_scope");
  ConcurrentMap<DexType*, Locations> concurrent_new_instances;
  ConcurrentMap<DexMethod*, Locations> concurrent_invokes;
  ConcurrentMap<DexMethod*, std::unordered_set<DexMethod*>>
      concurrent_dependencies;
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    code.build_cfg(/* editable */ true);
    for (auto& mie : InstructionIterable(code.cfg())) {
      auto insn = mie.insn;
      if (insn->opcode() == OPCODE_NEW_INSTANCE) {
        auto cls = type_class(insn->get_type());
        if (cls && !cls->is_external()) {
          concurrent_new_instances.update(
              insn->get_type(),
              [&](auto*, auto& vec, bool) { vec.emplace_back(method, insn); });
        }
      } else if (opcode::is_an_invoke(insn->opcode())) {
        auto callee =
            resolve_method(insn->get_method(), opcode_to_search(insn));
        if (callee &&
            (!callee->is_virtual() || non_overridden_virtuals.count(callee))) {
          concurrent_invokes.update(callee, [&](auto*, auto& vec, bool) {
            vec.emplace_back(method, insn);
          });
          if (!method->is_virtual() || non_overridden_virtuals.count(method)) {
            concurrent_dependencies.update(
                callee,
                [method](auto, auto& set, auto) { set.insert(method); });
          }
        }
      }
    }
  });
  *new_instances = concurrent_new_instances.move_to_container();
  *invokes = concurrent_invokes.move_to_container();
  *dependencies = concurrent_dependencies.move_to_container();
}

// A benign method invocation can be ignored during the escape analysis.
bool is_benign(const DexMethodRef* method_ref) {
  static const std::unordered_set<std::string> methods = {
      // clang-format off
      "Ljava/lang/Object;.<init>:()V",
      // clang-format on
  };

  return method_ref->is_def() &&
         methods.count(
             method_ref->as_def()->get_deobfuscated_name_or_empty_copy());
}

constexpr const IRInstruction* NO_ALLOCATION = nullptr;

using namespace ir_analyzer;

// For each allocating instruction that escapes (not including returns), all
// uses by which it escapes.
using Escapes = std::unordered_map<const IRInstruction*,
                                   std::unordered_set<live_range::Use>>;

// For each object, we track which instruction might have allocated it:
// - new-instance, invoke-, and load-param-object instructions might represent
//   allocation points
// - NO_ALLOCATION is a value for which the allocation instruction is not known,
//   or it is not an object
using Domain = sparta::PatriciaTreeSetAbstractDomain<const IRInstruction*>;

// For each register that holds a relevant value, keep track of it.
using Environment = sparta::PatriciaTreeMapAbstractEnvironment<reg_t, Domain>;

struct MethodSummary {
  // A parameter is "benign" if a provided argument does not escape
  std::unordered_set<src_index_t> benign_params;
  // A method might contain a unique instruction which allocates an object that
  // is eventually unconditionally returned.
  const IRInstruction* allocation_insn{nullptr};
};

using MethodSummaries = std::unordered_map<DexMethod*, MethodSummary>;

// The analyzer computes...
// - which instructions allocate (new-instance, invoke-)
// - which allocations escape (and how)
// - which allocations return
class Analyzer final : public BaseIRAnalyzer<Environment> {
 public:
  explicit Analyzer(const MethodSummaries& method_summaries,
                    DexMethodRef* incomplete_marker_method,
                    cfg::ControlFlowGraph& cfg)
      : BaseIRAnalyzer(cfg),
        m_method_summaries(method_summaries),
        m_incomplete_marker_method(incomplete_marker_method) {
    MonotonicFixpointIterator::run(Environment::top());
  }

  static const IRInstruction* get_singleton_allocation(const Domain& domain) {
    always_assert(domain.kind() == AbstractValueKind::Value);
    auto& elements = domain.elements();
    if (elements.size() != 1) {
      return nullptr;
    }
    return *elements.begin();
  }

  void analyze_instruction(const IRInstruction* insn,
                           Environment* current_state) const override {

    const auto escape = [&](src_index_t src_idx) {
      auto reg = insn->src(src_idx);
      const auto& domain = current_state->get(reg);
      always_assert(domain.kind() == AbstractValueKind::Value);
      for (auto allocation_insn : domain.elements()) {
        if (allocation_insn != NO_ALLOCATION) {
          m_escapes[allocation_insn].insert(
              {const_cast<IRInstruction*>(insn), src_idx});
        }
      }
    };

    if (insn->opcode() == OPCODE_NEW_INSTANCE) {
      auto type = insn->get_type();
      auto cls = type_class(type);
      if (cls && !cls->is_external()) {
        m_escapes[insn];
        current_state->set(RESULT_REGISTER, Domain(insn));
        return;
      }
    } else if (insn->opcode() == IOPCODE_LOAD_PARAM_OBJECT) {
      m_escapes[insn];
      current_state->set(insn->dest(), Domain(insn));
      return;
    } else if (insn->opcode() == OPCODE_RETURN_OBJECT) {
      const auto& domain = current_state->get(insn->src(0));
      always_assert(domain.kind() == AbstractValueKind::Value);
      m_returns.insert(domain.elements().begin(), domain.elements().end());
      return;
    } else if (insn->opcode() == OPCODE_MOVE_RESULT_OBJECT ||
               insn->opcode() == IOPCODE_MOVE_RESULT_PSEUDO_OBJECT) {
      const auto& domain = current_state->get(RESULT_REGISTER);
      current_state->set(insn->dest(), domain);
      return;
    } else if (insn->opcode() == OPCODE_MOVE_OBJECT) {
      const auto& domain = current_state->get(insn->src(0));
      current_state->set(insn->dest(), domain);
      return;
    } else if (insn->opcode() == OPCODE_INSTANCE_OF ||
               opcode::is_an_iget(insn->opcode())) {
      if (get_singleton_allocation(current_state->get(insn->src(0)))) {
        current_state->set(RESULT_REGISTER, Domain(NO_ALLOCATION));
        return;
      }
    } else if (opcode::is_a_monitor(insn->opcode()) ||
               insn->opcode() == OPCODE_IF_EQZ ||
               insn->opcode() == OPCODE_IF_NEZ) {
      if (get_singleton_allocation(current_state->get(insn->src(0)))) {
        return;
      }
    } else if (opcode::is_an_iput(insn->opcode())) {
      if (get_singleton_allocation(current_state->get(insn->src(1)))) {
        escape(0);
        return;
      }
    } else if (opcode::is_an_invoke(insn->opcode())) {
      if (is_benign(insn->get_method()) || is_incomplete_marker(insn)) {
        current_state->set(RESULT_REGISTER, Domain(NO_ALLOCATION));
        return;
      }
      auto callee = resolve_method(insn->get_method(), opcode_to_search(insn));
      auto it = m_method_summaries.find(callee);
      auto benign_params =
          it == m_method_summaries.end() ? nullptr : &it->second.benign_params;
      for (src_index_t i = 0; i < insn->srcs_size(); i++) {
        if (!benign_params || !benign_params->count(i) ||
            !get_singleton_allocation(current_state->get(insn->src(i)))) {
          escape(i);
        }
      }

      Domain domain(NO_ALLOCATION);
      if (it != m_method_summaries.end() && it->second.allocation_insn) {
        m_escapes[insn];
        domain = Domain(insn);
      }
      current_state->set(RESULT_REGISTER, domain);
      return;
    }

    for (src_index_t i = 0; i < insn->srcs_size(); i++) {
      escape(i);
    }

    if (insn->has_dest()) {
      current_state->set(insn->dest(), Domain(NO_ALLOCATION));
      if (insn->dest_is_wide()) {
        current_state->set(insn->dest() + 1, Domain::top());
      }
    } else if (insn->has_move_result_any()) {
      current_state->set(RESULT_REGISTER, Domain(NO_ALLOCATION));
    }
  }

  const Escapes& get_escapes() { return m_escapes; }

  const std::unordered_set<const IRInstruction*>& get_returns() {
    return m_returns;
  }

  // Returns set of new-instance and invoke- allocating instructions that do not
  // escape (or return).
  std::unordered_set<IRInstruction*> get_inlinables() {
    std::unordered_set<IRInstruction*> inlinables;
    for (auto&& [insn, uses] : m_escapes) {
      if (uses.empty() && insn->opcode() != IOPCODE_LOAD_PARAM_OBJECT &&
          !m_returns.count(insn)) {
        inlinables.insert(const_cast<IRInstruction*>(insn));
      }
    }
    return inlinables;
  }

 private:
  const MethodSummaries& m_method_summaries;
  DexMethodRef* m_incomplete_marker_method;
  mutable Escapes m_escapes;
  mutable std::unordered_set<const IRInstruction*> m_returns;

  bool is_incomplete_marker(const IRInstruction* insn) const {
    return insn->opcode() == OPCODE_INVOKE_STATIC &&
           insn->get_method() == m_incomplete_marker_method;
  }
};

MethodSummaries compute_method_summaries(
    PassManager& mgr,
    const Scope& scope,
    const std::unordered_map<DexMethod*, std::unordered_set<DexMethod*>>&
        dependencies,
    const std::unordered_set<const DexMethod*>& non_overridden_virtuals) {
  Timer t("compute_method_summaries");

  std::unordered_set<DexMethod*> impacted_methods;
  walk::code(scope, [&](DexMethod* method, IRCode&) {
    if (!method->is_virtual() || non_overridden_virtuals.count(method)) {
      impacted_methods.insert(method);
    }
  });

  MethodSummaries method_summaries;
  size_t analysis_iterations = 0;
  while (!impacted_methods.empty()) {
    Timer t2("analysis iteration");
    analysis_iterations++;
    TRACE(OEA, 2, "[object escape analysis] analysis_iteration %zu",
          analysis_iterations);
    ConcurrentMap<DexMethod*, MethodSummary> recomputed_method_summaries;
    workqueue_run<DexMethod*>(
        [&](DexMethod* method) {
          auto& cfg = method->get_code()->cfg();
          Analyzer analyzer(method_summaries,
                            /* incomplete_marker_method */ nullptr, cfg);
          const auto& escapes = analyzer.get_escapes();
          const auto& returns = analyzer.get_returns();
          src_index_t src_index = 0;
          for (auto& mie : InstructionIterable(cfg.get_param_instructions())) {
            if (mie.insn->opcode() == IOPCODE_LOAD_PARAM_OBJECT &&
                escapes.at(mie.insn).empty() && !returns.count(mie.insn)) {
              recomputed_method_summaries.update(
                  method, [src_index](DexMethod*, auto& ms, bool) {
                    ms.benign_params.insert(src_index);
                  });
            }
            src_index++;
          }
          const IRInstruction* allocation_insn;
          if (returns.size() == 1 &&
              (allocation_insn = *returns.begin()) != NO_ALLOCATION &&
              escapes.at(allocation_insn).empty() &&
              allocation_insn->opcode() != IOPCODE_LOAD_PARAM_OBJECT) {
            recomputed_method_summaries.update(
                method, [allocation_insn](DexMethod*, auto& ms, bool) {
                  ms.allocation_insn = allocation_insn;
                });
          }
        },
        impacted_methods);

    std::unordered_set<DexMethod*> changed_methods;
    // (Recomputed) summaries can only grow; assert that, update summaries when
    // necessary, and remember for which methods the summaries actually changed.
    for (auto&& [method, recomputed_summary] : recomputed_method_summaries) {
      auto& summary = method_summaries[method];
      for (auto src_index : summary.benign_params) {
        always_assert(recomputed_summary.benign_params.count(src_index));
      }
      if (recomputed_summary.benign_params.size() >
          summary.benign_params.size()) {
        summary.benign_params = std::move(recomputed_summary.benign_params);
        changed_methods.insert(method);
      }
      if (recomputed_summary.allocation_insn) {
        if (summary.allocation_insn) {
          always_assert(summary.allocation_insn ==
                        recomputed_summary.allocation_insn);
        } else {
          summary.allocation_insn = recomputed_summary.allocation_insn;
          changed_methods.insert(method);
        }
      } else {
        always_assert(summary.allocation_insn == nullptr);
      }
    }
    impacted_methods.clear();
    for (auto method : changed_methods) {
      auto it = dependencies.find(method);
      if (it != dependencies.end()) {
        impacted_methods.insert(it->second.begin(), it->second.end());
      }
    }
  }
  mgr.incr_metric("analysis_iterations", analysis_iterations);
  return method_summaries;
}

// For an inlinable new-instance or invoke- instruction, determine first
// resolved callee (if any), and (eventually) allocated type
std::pair<DexMethod*, DexType*> resolve_inlinable(
    const MethodSummaries& method_summaries, const IRInstruction* insn) {
  always_assert(insn->opcode() == OPCODE_NEW_INSTANCE ||
                opcode::is_an_invoke(insn->opcode()));
  DexMethod* first_callee{nullptr};
  while (insn->opcode() != OPCODE_NEW_INSTANCE) {
    always_assert(opcode::is_an_invoke(insn->opcode()));
    auto callee = resolve_method(insn->get_method(), opcode_to_search(insn));
    if (!first_callee) {
      first_callee = callee;
    }
    insn = method_summaries.at(callee).allocation_insn;
  }
  return std::make_pair(first_callee, insn->get_type());
}

using InlineAnchorsOfType =
    std::unordered_map<DexMethod*, std::unordered_set<IRInstruction*>>;
std::unordered_map<DexType*, InlineAnchorsOfType> compute_inline_anchors(
    const Scope& scope, const MethodSummaries& method_summaries) {
  Timer t("compute_inline_anchors");
  ConcurrentMap<DexType*, InlineAnchorsOfType> concurrent_inline_anchors;
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    Analyzer analyzer(method_summaries, /* incomplete_marker_method */ nullptr,
                      code.cfg());
    auto inlinables = analyzer.get_inlinables();
    for (auto insn : inlinables) {
      auto [callee, type] = resolve_inlinable(method_summaries, insn);
      TRACE(OEA, 3, "[object escape analysis] inline anchor [%s] %s",
            SHOW(method), SHOW(insn));
      concurrent_inline_anchors.update(
          type, [&](auto*, auto& map, bool) { map[method].insert(insn); });
    }
  });
  return concurrent_inline_anchors.move_to_container();
}

class InlinedCodeSizeEstimator {
 private:
  using DeltaKey = std::pair<DexMethod*, const IRInstruction*>;
  LazyUnorderedMap<DexMethod*, size_t> m_inlined_code_sizes;
  LazyUnorderedMap<DexMethod*, live_range::DefUseChains> m_du_chains;
  LazyUnorderedMap<DeltaKey, float, boost::hash<DeltaKey>> m_deltas;

 public:
  explicit InlinedCodeSizeEstimator(const MethodSummaries& method_summaries)
      : m_inlined_code_sizes([](DexMethod* method) {
          size_t code_size{0};
          auto& cfg = method->get_code()->cfg();
          for (auto& mie : InstructionIterable(cfg)) {
            auto* insn = mie.insn;
            // We do the cost estimation before shrinking, so we discount moves.
            // Returns will go away after inlining.
            if (opcode::is_a_move(insn->opcode()) ||
                opcode::is_a_return(insn->opcode())) {
              continue;
            }
            code_size += mie.insn->size();
          }
          return code_size;
        }),
        m_du_chains([](DexMethod* method) {
          live_range::MoveAwareChains chains(method->get_code()->cfg());
          return chains.get_def_use_chains();
        }),
        m_deltas([&](DeltaKey key) {
          auto [method, allocation_insn] = key;
          float delta = 0;
          auto& du_chains = m_du_chains[method];
          if (opcode::is_an_invoke(allocation_insn->opcode())) {
            auto callee = resolve_method(allocation_insn->get_method(),
                                         opcode_to_search(allocation_insn));
            always_assert(callee);
            auto* callee_allocation_insn =
                method_summaries.at(callee).allocation_insn;
            always_assert(callee_allocation_insn);
            delta += m_inlined_code_sizes[callee] +
                     get_delta(callee, callee_allocation_insn) - COST_INVOKE -
                     COST_MOVE_RESULT;
          } else if (allocation_insn->opcode() == OPCODE_NEW_INSTANCE) {
            delta -= COST_NEW_INSTANCE;
          }
          for (auto& use :
               du_chains[const_cast<IRInstruction*>(allocation_insn)]) {
            if (opcode::is_an_invoke(use.insn->opcode())) {
              delta -= COST_INVOKE;
              auto callee = resolve_method(use.insn->get_method(),
                                           opcode_to_search(use.insn));
              always_assert(callee);
              if (is_benign(callee)) {
                continue;
              }
              auto load_param_insns = InstructionIterable(
                  callee->get_code()->cfg().get_param_instructions());
              auto* load_param_insn =
                  std::next(load_param_insns.begin(), use.src_index)->insn;
              always_assert(load_param_insn);
              always_assert(opcode::is_a_load_param(load_param_insn->opcode()));
              delta += m_inlined_code_sizes[callee] +
                       get_delta(callee, load_param_insn);
              if (!callee->get_proto()->is_void()) {
                delta -= COST_MOVE_RESULT;
              }
            } else if (opcode::is_an_iget(use.insn->opcode()) ||
                       opcode::is_an_iput(use.insn->opcode())) {
              delta -= use.insn->size();
            }
          }
          return delta;
        }) {}

  float get_delta(DexMethod* method, const IRInstruction* allocation_insn) {
    return m_deltas[std::make_pair(method, allocation_insn)];
  }
};

enum class InlinableTypeKind {
  // All uses of this type have inline anchors in identified root methods. It is
  // safe to inline all resolved inlinables.
  CompleteSingleRoot,
  // Same as CompleteSingleRoot, except that inlinable anchors are spread over
  // multiple root methods.
  CompleteMultipleRoots,
  // Not all uses of this type have inlinable anchors. Only attempt to inline
  // anchors present in original identified root methods.
  Incomplete,

  Last = Incomplete
};
using InlinableTypes = std::unordered_map<DexType*, InlinableTypeKind>;

std::unordered_map<DexMethod*, InlinableTypes> compute_root_methods(
    PassManager& mgr,
    const std::unordered_map<DexType*, Locations>& new_instances,
    const std::unordered_map<DexMethod*, Locations>& invokes,
    const MethodSummaries& method_summaries,
    const std::unordered_map<DexType*, InlineAnchorsOfType>& inline_anchors) {
  Timer t("compute_root_methods");
  std::array<size_t, (size_t)(InlinableTypeKind::Last) + 1> candidate_types{
      0, 0, 0};
  std::unordered_map<DexMethod*, InlinableTypes> root_methods;
  std::unordered_set<DexType*> inline_anchor_types;

  std::mutex mutex; // protects candidate_types and root_methods
  std::atomic<size_t> incomplete_estimated_delta_threshold_exceeded{0};
  auto concurrent_add_root_methods = [&](DexType* type, bool complete) {
    const auto& inline_anchors_of_type = inline_anchors.at(type);
    InlinedCodeSizeEstimator inlined_code_size_estimator(method_summaries);
    std::vector<DexMethod*> methods;
    for (auto& [method, allocation_insns] : inline_anchors_of_type) {
      auto it2 = method_summaries.find(method);
      if (it2 != method_summaries.end() && it2->second.allocation_insn &&
          resolve_inlinable(method_summaries, it2->second.allocation_insn)
                  .second == type) {
        continue;
      }
      if (!complete) {
        float delta = 0;
        for (auto allocation_insn : allocation_insns) {
          delta +=
              inlined_code_size_estimator.get_delta(method, allocation_insn);
        }
        if (delta > INCOMPLETE_ESTIMATED_DELTA_THRESHOLD) {
          // Skipping, as it's highly unlikely to results in an overall size
          // win, while taking a very long time to compute exactly.
          incomplete_estimated_delta_threshold_exceeded++;
          continue;
        }
      }
      methods.push_back(method);
    }
    if (methods.empty()) {
      return;
    }
    bool multiple_roots = methods.size() > 1;
    auto kind = complete ? multiple_roots
                               ? InlinableTypeKind::CompleteMultipleRoots
                               : InlinableTypeKind::CompleteSingleRoot
                         : InlinableTypeKind::Incomplete;

    std::lock_guard<std::mutex> lock_guard(mutex);
    candidate_types[(size_t)kind]++;
    for (auto method : methods) {
      TRACE(OEA, 3, "[object escape analysis] root method %s with %s%s",
            SHOW(method), SHOW(type),
            kind == InlinableTypeKind::CompleteMultipleRoots
                ? " complete multiple-roots"
            : kind == InlinableTypeKind::CompleteSingleRoot
                ? " complete single-root"
                : " incomplete");
      root_methods[method].emplace(type, kind);
    }
  };

  for (auto& [type, method_insn_pairs] : new_instances) {
    auto it = inline_anchors.find(type);
    if (it == inline_anchors.end()) {
      continue;
    }
    inline_anchor_types.insert(type);
  }
  workqueue_run<DexType*>(
      [&](DexType* type) {
        const auto& method_insn_pairs = new_instances.at(type);
        const auto& inline_anchors_of_type = inline_anchors.at(type);

        std::function<bool(const std::pair<DexMethod*, const IRInstruction*>&)>
            is_anchored;
        is_anchored = [&](const auto& p) {
          auto [method, insn] = p;
          auto it2 = inline_anchors_of_type.find(method);
          if (it2 != inline_anchors_of_type.end() &&
              it2->second.count(const_cast<IRInstruction*>(insn))) {
            return true;
          }
          auto it3 = method_summaries.find(method);
          if (it3 == method_summaries.end() ||
              it3->second.allocation_insn != insn) {
            return false;
          }
          auto it4 = invokes.find(method);
          if (it4 == invokes.end()) {
            return false;
          }
          for (auto q : it4->second) {
            if (!is_anchored(q)) {
              return false;
            }
          }
          return true;
        };

        // Check whether all uses of this type have inline anchors optimizable
        // root methods.
        bool complete =
            std::all_of(method_insn_pairs.begin(), method_insn_pairs.end(),
                        is_anchored) &&
            std::all_of(
                inline_anchors_of_type.begin(), inline_anchors_of_type.end(),
                [](auto& p) { return !p.first->rstate.no_optimizations(); });

        concurrent_add_root_methods(type, complete);
      },
      inline_anchor_types);

  TRACE(OEA,
        1,
        "[object escape analysis] candidate types: %zu",
        candidate_types.size());
  mgr.incr_metric(
      "candidate types CompleteSingleRoot",
      candidate_types[(size_t)InlinableTypeKind::CompleteSingleRoot]);
  mgr.incr_metric(
      "candidate types CompleteMultipleRoots",
      candidate_types[(size_t)InlinableTypeKind::CompleteMultipleRoots]);
  mgr.incr_metric("candidate types Incomplete",
                  candidate_types[(size_t)InlinableTypeKind::Incomplete]);
  mgr.incr_metric("incomplete_estimated_delta_threshold_exceeded",
                  (size_t)incomplete_estimated_delta_threshold_exceeded);
  return root_methods;
}

size_t get_code_size(DexMethod* method) {
  return method->get_code()->cfg().sum_opcode_sizes();
}

struct Stats {
  std::atomic<size_t> total_savings{0};
  std::atomic<size_t> reduced_methods{0};
  std::atomic<size_t> reduced_methods_variants{0};
  std::atomic<size_t> selected_reduced_methods{0};
  std::atomic<size_t> invokes_not_inlinable_callee_unexpandable{0};
  std::atomic<size_t> invokes_not_inlinable_callee_inconcrete{0};
  std::atomic<size_t> invokes_not_inlinable_callee_too_many_params_to_expand{0};
  std::atomic<size_t> invokes_not_inlinable_inlining{0};
  std::atomic<size_t> invokes_not_inlinable_too_many_iterations{0};
  std::atomic<size_t> anchors_not_inlinable_inlining{0};
  std::atomic<size_t> stackify_returns_objects{0};
  std::atomic<size_t> too_costly_multiple_conditional_classes{0};
  std::atomic<size_t> too_costly_irreducible_classes{0};
  std::atomic<size_t> too_costly_globally{0};
  std::atomic<size_t> expanded_methods{0};
  std::atomic<size_t> calls_inlined{0};
  std::atomic<size_t> new_instances_eliminated{0};
};

// Data structure to derive local or accumulated global net savings
struct NetSavings {
  int local{0};
  std::unordered_set<DexType*> classes;
  std::unordered_set<DexMethod*> methods;

  NetSavings& operator+=(const NetSavings& other) {
    local += other.local;
    classes.insert(other.classes.begin(), other.classes.end());
    methods.insert(other.methods.begin(), other.methods.end());
    return *this;
  }

  // Estimate how many code units will be saved.
  int get_value() const {
    int net_savings{local};
    // A class will only eventually get deleted if all static methods are
    // inlined.
    std::unordered_map<DexType*, std::unordered_set<DexMethod*>> smethods;
    for (auto type : classes) {
      auto cls = type_class(type);
      always_assert(cls);
      auto& type_smethods = smethods[type];
      for (auto method : cls->get_dmethods()) {
        if (is_static(method)) {
          type_smethods.insert(method);
        }
      }
    }
    for (auto method : methods) {
      if (can_delete(method)) {
        auto code_size = get_code_size(method);
        net_savings += COST_METHOD + code_size;
        if (is_static(method)) {
          smethods[method->get_class()].erase(method);
        }
      }
    }
    for (auto type : classes) {
      auto cls = type_class(type);
      always_assert(cls);
      if (can_delete(cls) && cls->get_sfields().empty() && !cls->get_clinit()) {
        auto& type_smethods = smethods[type];
        if (type_smethods.empty()) {
          net_savings += COST_CLASS;
        }
      }
      for (auto field : cls->get_ifields()) {
        if (can_delete(field)) {
          net_savings += COST_FIELD;
        }
      }
    }
    return net_savings;
  }
};

// Data structure that represents a (cloned) reduced method, together with some
// auxiliary information that allows to derive net savings.
struct ReducedMethod {
  DexMethod* method;
  size_t initial_code_size;
  std::unordered_map<DexMethod*, std::unordered_set<DexType*>> inlined_methods;
  InlinableTypes types;
  size_t calls_inlined;
  size_t new_instances_eliminated;

  NetSavings get_net_savings(
      const std::unordered_set<DexType*>& irreducible_types,
      NetSavings* conditional_net_savings = nullptr) const {
    auto final_code_size = get_code_size(method);
    NetSavings net_savings;
    net_savings.local = (int)initial_code_size - (int)final_code_size;

    std::unordered_set<DexType*> remaining;
    for (auto& [inlined_method, inlined_types] : inlined_methods) {
      bool any_remaining = false;
      bool any_incomplete = false;
      for (auto type : inlined_types) {
        auto kind = types.at(type);
        if (kind != InlinableTypeKind::CompleteSingleRoot ||
            irreducible_types.count(type)) {
          remaining.insert(type);
          any_remaining = true;
          if (kind == InlinableTypeKind::Incomplete) {
            any_incomplete = true;
          }
        }
      }
      if (any_remaining) {
        if (conditional_net_savings && !any_incomplete) {
          conditional_net_savings->methods.insert(inlined_method);
        }
        continue;
      }
      net_savings.methods.insert(inlined_method);
    }

    for (auto [type, kind] : types) {
      if (remaining.count(type) ||
          kind != InlinableTypeKind::CompleteSingleRoot ||
          irreducible_types.count(type)) {
        if (conditional_net_savings && kind != InlinableTypeKind::Incomplete) {
          conditional_net_savings->classes.insert(type);
        }
        continue;
      }
      net_savings.classes.insert(type);
    }

    return net_savings;
  }
};

class RootMethodReducer {
 private:
  const ExpandableMethodParams& m_expandable_method_params;
  DexMethodRef* m_incomplete_marker_method;
  MultiMethodInliner& m_inliner;
  const MethodSummaries& m_method_summaries;
  Stats* m_stats;
  bool m_is_init_or_clinit;
  DexMethod* m_method;
  const InlinableTypes& m_types;
  size_t m_calls_inlined{0};
  size_t m_new_instances_eliminated{0};
  size_t m_max_inline_size;

 public:
  RootMethodReducer(const ExpandableMethodParams& expandable_method_params,
                    DexMethodRef* incomplete_marker_method,
                    MultiMethodInliner& inliner,
                    const MethodSummaries& method_summaries,
                    Stats* stats,
                    bool is_init_or_clinit,
                    DexMethod* method,
                    const InlinableTypes& types,
                    size_t max_inline_size)
      : m_expandable_method_params(expandable_method_params),
        m_incomplete_marker_method(incomplete_marker_method),
        m_inliner(inliner),
        m_method_summaries(method_summaries),
        m_stats(stats),
        m_is_init_or_clinit(is_init_or_clinit),
        m_method(method),
        m_types(types),
        m_max_inline_size(max_inline_size) {}

  std::optional<ReducedMethod> reduce() {
    auto initial_code_size{get_code_size(m_method)};

    if (!inline_anchors() || !expand_or_inline_invokes()) {
      return std::nullopt;
    }

    while (auto opt_p = find_inlinable_new_instance()) {
      if (!stackify(opt_p->first, opt_p->second)) {
        return std::nullopt;
      }
    }

    auto* insn = find_incomplete_marker_methods();
    always_assert_log(!insn,
                      "Incomplete marker {%s} present after reduction in\n%s",
                      SHOW(insn), SHOW(m_method->get_code()->cfg()));

    shrink();
    return (ReducedMethod){
        m_method, initial_code_size, std::move(m_inlined_methods),
        m_types,  m_calls_inlined,   m_new_instances_eliminated};
  }

 private:
  void shrink() {
    m_inliner.get_shrinker().shrink_code(m_method->get_code(),
                                         is_static(m_method),
                                         m_is_init_or_clinit,
                                         m_method->get_class(),
                                         m_method->get_proto(),
                                         [this]() { return show(m_method); });
  }

  bool inline_insns(const std::unordered_set<IRInstruction*>& insns) {
    auto inlined = m_inliner.inline_callees(m_method, insns);
    m_calls_inlined += inlined;
    return inlined == insns.size();
  }

  // Given a method invocation, replace a particular argument with the
  // sequence of the argument's field values to flow into an expanded
  // method.
  DexMethodRef* expand_invoke(cfg::CFGMutation& mutation,
                              const cfg::InstructionIterator& it,
                              param_index_t param_index) {
    auto insn = it->insn;
    auto callee = resolve_method(insn->get_method(), opcode_to_search(insn));
    always_assert(callee);
    always_assert(callee->is_concrete());
    std::vector<DexField*>* fields;
    auto expanded_method_ref =
        m_expandable_method_params.get_expanded_method_ref(callee, param_index,
                                                           &fields);
    if (!expanded_method_ref) {
      return nullptr;
    }

    insn->set_method(expanded_method_ref);
    if (!method::is_init(expanded_method_ref)) {
      insn->set_opcode(OPCODE_INVOKE_STATIC);
    }
    auto obj_reg = insn->src(param_index);
    auto srcs_range = insn->srcs();
    std::vector<reg_t> srcs_copy(srcs_range.begin(), srcs_range.end());
    insn->set_srcs_size(srcs_copy.size() - 1 + fields->size());
    for (param_index_t i = param_index; i < srcs_copy.size() - 1; i++) {
      insn->set_src(i + fields->size(), srcs_copy.at(i + 1));
    }
    std::vector<IRInstruction*> instructions_to_insert;
    auto& cfg = m_method->get_code()->cfg();
    for (auto field : *fields) {
      auto reg = type::is_wide_type(field->get_type())
                     ? cfg.allocate_wide_temp()
                     : cfg.allocate_temp();
      insn->set_src(param_index++, reg);
      auto iget_opcode = opcode::iget_opcode_for_field(field);
      instructions_to_insert.push_back((new IRInstruction(iget_opcode))
                                           ->set_src(0, obj_reg)
                                           ->set_field(field));
      auto move_result_pseudo_opcode =
          opcode::move_result_pseudo_for_iget(iget_opcode);
      instructions_to_insert.push_back(
          (new IRInstruction(move_result_pseudo_opcode))->set_dest(reg));
    }
    mutation.insert_before(it, std::move(instructions_to_insert));
    return expanded_method_ref;
  }

  bool expand_invokes(const std::unordered_map<IRInstruction*, param_index_t>&
                          invokes_to_expand,
                      std::unordered_set<DexMethodRef*>* expanded_method_refs) {
    if (invokes_to_expand.empty()) {
      return true;
    }
    auto& cfg = m_method->get_code()->cfg();
    cfg::CFGMutation mutation(cfg);
    auto ii = InstructionIterable(cfg);
    for (auto it = ii.begin(); it != ii.end(); it++) {
      auto insn = it->insn;
      auto it2 = invokes_to_expand.find(insn);
      if (it2 == invokes_to_expand.end()) {
        continue;
      }
      auto param_index = it2->second;
      auto expanded_method_ref = expand_invoke(mutation, it, param_index);
      if (!expanded_method_ref) {
        return false;
      }
      expanded_method_refs->insert(expanded_method_ref);
    }
    mutation.flush();
    return true;
  }

  // Inline all "anchors" until all relevant allocations are new-instance
  // instructions in the (root) method.
  bool inline_anchors() {
    auto& cfg = m_method->get_code()->cfg();
    for (int iteration = 0; true; iteration++) {
      Analyzer analyzer(m_method_summaries, m_incomplete_marker_method, cfg);
      std::unordered_set<IRInstruction*> invokes_to_inline;
      auto inlinables = analyzer.get_inlinables();
      Lazy<live_range::DefUseChains> du_chains([&]() {
        live_range::MoveAwareChains chains(cfg);
        return chains.get_def_use_chains();
      });
      cfg::CFGMutation mutation(cfg);
      for (auto insn : inlinables) {
        auto [callee, type] = resolve_inlinable(m_method_summaries, insn);
        auto it = m_types.find(type);
        if (it == m_types.end()) {
          continue;
        }
        if (it->second == InlinableTypeKind::Incomplete) {
          // We are only going to consider incompletely inlinable types when we
          // find them in the first iteration, i.e. in the original method,
          // and not coming from any inlined method. We are then going insert
          // a special marker invocation instruction so that we can later find
          // the originally matched anchors again. This instruction will get
          // removed later.
          if (!has_incomplete_marker((*du_chains)[insn])) {
            if (iteration > 0) {
              continue;
            }
            auto insn_it = cfg.find_insn(insn);
            always_assert(!insn_it.is_end());
            auto move_result_it = cfg.move_result_of(insn_it);
            if (move_result_it.is_end()) {
              continue;
            }
            auto invoke_insn = (new IRInstruction(OPCODE_INVOKE_STATIC))
                                   ->set_method(m_incomplete_marker_method)
                                   ->set_srcs_size(1)
                                   ->set_src(0, move_result_it->insn->dest());
            mutation.insert_after(move_result_it, {invoke_insn});
          }
        }
        if (!callee) {
          continue;
        }
        invokes_to_inline.insert(insn);
        m_inlined_methods[callee].insert(type);
      }
      mutation.flush();
      if (invokes_to_inline.empty()) {
        return true;
      }
      if (!inline_insns(invokes_to_inline)) {
        m_stats->anchors_not_inlinable_inlining++;
        return false;
      }
      // simplify to prune now unreachable code, e.g. from removed exception
      // handlers
      cfg.simplify();
    }
  }

  bool is_inlinable_new_instance(IRInstruction* insn) const {
    return insn->opcode() == OPCODE_NEW_INSTANCE &&
           m_types.count(insn->get_type());
  }

  bool is_incomplete_marker(IRInstruction* insn) const {
    return insn->opcode() == OPCODE_INVOKE_STATIC &&
           insn->get_method() == m_incomplete_marker_method;
  }

  bool has_incomplete_marker(
      const std::unordered_set<live_range::Use>& uses) const {
    return std::any_of(uses.begin(), uses.end(), [&](auto& use) {
      return is_incomplete_marker(use.insn);
    });
  }

  IRInstruction* find_incomplete_marker_methods() {
    auto& cfg = m_method->get_code()->cfg();
    for (auto& mie : InstructionIterable(cfg)) {
      if (is_incomplete_marker(mie.insn)) {
        return mie.insn;
      }
    }
    return nullptr;
  }

  std::optional<std::pair<live_range::DefUseChains, IRInstruction*>>
  find_inlinable_new_instance() const {
    auto& cfg = m_method->get_code()->cfg();
    Lazy<live_range::DefUseChains> du_chains([&]() {
      live_range::MoveAwareChains chains(cfg);
      return chains.get_def_use_chains();
    });
    for (auto& mie : InstructionIterable(cfg)) {
      auto insn = mie.insn;
      if (!is_inlinable_new_instance(insn)) {
        continue;
      }
      auto type = insn->get_type();
      if (m_types.at(type) == InlinableTypeKind::Incomplete &&
          !has_incomplete_marker((*du_chains)[insn])) {
        continue;
      }
      return std::make_optional(std::pair(std::move(*du_chains), insn));
    }
    return std::nullopt;
  }

  bool should_expand(
      DexMethod* callee,
      const std::unordered_map<src_index_t, DexType*>& src_indices) {
    always_assert(!src_indices.empty());
    if (method::is_init(callee) && !src_indices.count(0)) {
      return true;
    }
    if (src_indices.size() > 1) {
      return false;
    }
    auto [param_index, type] = *src_indices.begin();
    bool multiples =
        m_types.at(type) == InlinableTypeKind::CompleteMultipleRoots;
    if (multiples && get_code_size(callee) > m_max_inline_size &&
        m_expandable_method_params.get_expanded_method_ref(callee,
                                                           param_index)) {
      return true;
    }
    return false;
  }

  // Expand or inline all uses of all relevant new-instance instructions that
  // involve invoke- instructions, until there are no more such uses.
  bool expand_or_inline_invokes() {
    auto& cfg = m_method->get_code()->cfg();
    std::unordered_set<DexMethodRef*> expanded_method_refs;
    for (int iteration = 0; iteration < MAX_INLINE_INVOKES_ITERATIONS;
         iteration++) {
      std::unordered_set<IRInstruction*> invokes_to_inline;
      std::unordered_map<IRInstruction*, param_index_t> invokes_to_expand;

      live_range::MoveAwareChains chains(cfg);
      auto du_chains = chains.get_def_use_chains();
      std::unordered_map<IRInstruction*,
                         std::unordered_map<src_index_t, DexType*>>
          aggregated_uses;
      for (auto& [insn, uses] : du_chains) {
        if (!is_inlinable_new_instance(insn)) {
          continue;
        }
        auto type = insn->get_type();
        if (m_types.at(type) == InlinableTypeKind::Incomplete &&
            !has_incomplete_marker(uses)) {
          continue;
        }
        for (auto& use : uses) {
          auto emplaced =
              aggregated_uses[use.insn].emplace(use.src_index, type).second;
          always_assert(emplaced);
        }
      }
      for (auto&& [uses_insn, src_indices] : aggregated_uses) {
        if (!opcode::is_an_invoke(uses_insn->opcode()) ||
            is_benign(uses_insn->get_method()) ||
            is_incomplete_marker(uses_insn)) {
          continue;
        }
        if (expanded_method_refs.count(uses_insn->get_method())) {
          m_stats->invokes_not_inlinable_callee_inconcrete++;
          return false;
        }
        auto callee = resolve_method(uses_insn->get_method(),
                                     opcode_to_search(uses_insn));
        always_assert(callee);
        always_assert(callee->is_concrete());
        if (should_expand(callee, src_indices)) {
          if (src_indices.size() > 1) {
            m_stats->invokes_not_inlinable_callee_too_many_params_to_expand++;
            return false;
          }
          always_assert(src_indices.size() == 1);
          auto src_index = src_indices.begin()->first;
          invokes_to_expand.emplace(uses_insn, src_index);
          continue;
        }

        invokes_to_inline.insert(uses_insn);
        for (auto [src_index, type] : src_indices) {
          m_inlined_methods[callee].insert(type);
        }
      }

      if (!expand_invokes(invokes_to_expand, &expanded_method_refs)) {
        m_stats->invokes_not_inlinable_callee_unexpandable++;
        return false;
      }

      if (invokes_to_inline.empty()) {
        // Nothing else to do
        return true;
      }
      if (!inline_insns(invokes_to_inline)) {
        m_stats->invokes_not_inlinable_inlining++;
        return false;
      }
      // simplify to prune now unreachable code, e.g. from removed exception
      // handlers
      cfg.simplify();
    }

    m_stats->invokes_not_inlinable_too_many_iterations++;
    return false;
  }

  // Given a new-instance instruction whose (main) uses are as the receiver in
  // iget- and iput- instruction, transform all such field accesses into
  // accesses to registers, one per field.
  bool stackify(live_range::DefUseChains& du_chains,
                IRInstruction* new_instance_insn) {
    auto& cfg = m_method->get_code()->cfg();
    std::unordered_map<DexField*, reg_t> field_regs;
    std::vector<DexField*> ordered_fields;
    auto get_field_reg = [&](DexFieldRef* ref) {
      always_assert(ref->is_def());
      auto field = ref->as_def();
      auto it = field_regs.find(field);
      if (it == field_regs.end()) {
        auto wide = type::is_wide_type(field->get_type());
        auto reg = wide ? cfg.allocate_wide_temp() : cfg.allocate_temp();
        it = field_regs.emplace(field, reg).first;
        ordered_fields.push_back(field);
      }
      return it->second;
    };

    auto& uses = du_chains[new_instance_insn];
    std::unordered_set<IRInstruction*> instructions_to_replace;
    bool identity_matters{false};
    for (auto& use : uses) {
      auto opcode = use.insn->opcode();
      if (opcode::is_an_iput(opcode)) {
        always_assert(use.src_index == 1);
      } else if (opcode::is_an_invoke(opcode) || opcode::is_a_monitor(opcode)) {
        always_assert(use.src_index == 0);
      } else if (opcode == OPCODE_IF_EQZ || opcode == OPCODE_IF_NEZ) {
        identity_matters = true;
        continue;
      } else if (opcode::is_move_object(opcode)) {
        continue;
      } else if (opcode::is_return_object(opcode)) {
        // Can happen if the root method is also an allocator
        m_stats->stackify_returns_objects++;
        return false;
      } else {
        always_assert_log(
            opcode::is_an_iget(opcode) || opcode::is_instance_of(opcode),
            "Unexpected use: %s at %u", SHOW(use.insn), use.src_index);
      }
      instructions_to_replace.insert(use.insn);
    }

    cfg::CFGMutation mutation(cfg);
    auto ii = InstructionIterable(cfg);
    auto new_instance_insn_it = ii.end();
    for (auto it = ii.begin(); it != ii.end(); it++) {
      auto insn = it->insn;
      if (!instructions_to_replace.count(insn)) {
        if (insn == new_instance_insn) {
          new_instance_insn_it = it;
        }
        continue;
      }
      auto opcode = insn->opcode();
      if (opcode::is_an_iget(opcode)) {
        auto move_result_it = cfg.move_result_of(it);
        auto new_insn = (new IRInstruction(opcode::iget_to_move(opcode)))
                            ->set_src(0, get_field_reg(insn->get_field()))
                            ->set_dest(move_result_it->insn->dest());
        mutation.replace(it, {new_insn});
      } else if (opcode::is_an_iput(opcode)) {
        auto new_insn = (new IRInstruction(opcode::iput_to_move(opcode)))
                            ->set_src(0, insn->src(0))
                            ->set_dest(get_field_reg(insn->get_field()));
        mutation.replace(it, {new_insn});
      } else if (opcode::is_an_invoke(opcode)) {
        always_assert(is_benign(insn->get_method()) ||
                      is_incomplete_marker(insn));
        if (is_incomplete_marker(insn) || !identity_matters) {
          mutation.remove(it);
        }
      } else if (opcode::is_instance_of(opcode)) {
        auto move_result_it = cfg.move_result_of(it);
        auto new_insn =
            (new IRInstruction(OPCODE_CONST))
                ->set_literal(type::is_subclass(insn->get_type(),
                                                new_instance_insn->get_type()))
                ->set_dest(move_result_it->insn->dest());
        mutation.replace(it, {new_insn});
      } else if (opcode::is_a_monitor(opcode)) {
        mutation.remove(it);
      } else {
        not_reached();
      }
    }

    always_assert(!new_instance_insn_it.is_end());
    auto init_class_insn =
        m_inliner.get_shrinker()
            .get_init_classes_with_side_effects()
            .create_init_class_insn(new_instance_insn->get_type());
    if (init_class_insn) {
      mutation.insert_before(new_instance_insn_it, {init_class_insn});
    }
    if (identity_matters) {
      new_instance_insn_it->insn->set_type(type::java_lang_Object());
    } else {
      auto move_result_it = cfg.move_result_of(new_instance_insn_it);
      auto new_insn = (new IRInstruction(OPCODE_CONST))
                          ->set_literal(0)
                          ->set_dest(move_result_it->insn->dest());
      mutation.replace(new_instance_insn_it, {new_insn});
    }

    // Insert zero-initialization code for field registers.

    std::sort(ordered_fields.begin(), ordered_fields.end(), compare_dexfields);

    std::vector<IRInstruction*> field_inits;
    field_inits.reserve(ordered_fields.size());
    for (auto field : ordered_fields) {
      auto wide = type::is_wide_type(field->get_type());
      auto opcode = wide ? OPCODE_CONST_WIDE : OPCODE_CONST;
      auto reg = field_regs.at(field);
      auto new_insn =
          (new IRInstruction(opcode))->set_literal(0)->set_dest(reg);
      field_inits.push_back(new_insn);
    }

    mutation.insert_before(new_instance_insn_it, field_inits);
    mutation.flush();
    // simplify to prune now unreachable code, e.g. from removed exception
    // handlers
    cfg.simplify();
    m_new_instances_eliminated++;
    return true;
  }

  std::unordered_map<DexMethod*, std::unordered_set<DexType*>>
      m_inlined_methods;
};

// Reduce all root methods to a set of variants. The reduced methods are ordered
// by how many types where inlined, with the largest number of inlined types
// going first.
std::unordered_map<DexMethod*, std::vector<ReducedMethod>>
compute_reduced_methods(
    ExpandableMethodParams& expandable_method_params,
    MultiMethodInliner& inliner,
    const MethodSummaries& method_summaries,
    const std::unordered_map<DexMethod*, InlinableTypes>& root_methods,
    std::unordered_set<DexType*>* irreducible_types,
    Stats* stats,
    size_t max_inline_size) {
  Timer t("compute_reduced_methods");

  // We are not exploring all possible subsets of types, but only single chain
  // of subsets, guided by the inlinable kind, and by how often
  // they appear as inlinable types in root methods.
  // TODO: Explore all possible subsets of types.
  std::unordered_map<DexType*, size_t> occurrences;
  for (auto&& [method, types] : root_methods) {
    for (auto [type, kind] : types) {
      occurrences[type]++;
    }
  }

  workqueue_run<std::pair<DexMethod*, InlinableTypes>>(
      [&](const std::pair<DexMethod*, InlinableTypes>& p) {
        const auto& [method, types] = p;
        inliner.get_shrinker().shrink_method(method);
      },
      root_methods);

  // This comparison function implies the sequence of inlinable type subsets
  // we'll consider. We'll structure the sequence such that often occurring
  // types with multiple uses will be chopped off the type set first.
  auto less = [&occurrences](auto& p, auto& q) {
    // We sort types with incomplete anchors to the front.
    if (p.second != q.second) {
      return p.second > q.second;
    }
    // We sort types with more frequent occurrences to the front.
    auto p_count = occurrences.at(p.first);
    auto q_count = occurrences.at(q.first);
    if (p_count != q_count) {
      return p_count > q_count;
    }
    // Tie breaker.
    return compare_dextypes(p.first, q.first);
  };

  // We'll now compute the set of variants we'll consider. For each root method
  // with N inlinable types, there will be N variants.
  std::vector<std::pair<DexMethod*, InlinableTypes>>
      ordered_root_methods_variants;
  for (auto&& [method, types] : root_methods) {
    std::vector<std::pair<DexType*, InlinableTypeKind>> ordered_types(
        types.begin(), types.end());
    std::sort(ordered_types.begin(), ordered_types.end(), less);
    for (auto it = ordered_types.begin(); it != ordered_types.end(); it++) {
      ordered_root_methods_variants.emplace_back(
          method, InlinableTypes(it, ordered_types.end()));
    }
  }
  // Order such that items with many types to process go first, which improves
  // workqueue efficiency.
  std::stable_sort(ordered_root_methods_variants.begin(),
                   ordered_root_methods_variants.end(), [](auto& a, auto& b) {
                     return a.second.size() > b.second.size();
                   });

  // Special marker method, used to identify which newly created objects of only
  // incompletely inlinable types should get inlined.
  DexMethodRef* incomplete_marker_method = DexMethod::make_method(
      "Lredex/$ObjectEscapeAnalysis;.markIncomplete:(Ljava/lang/Object;)V");

  // We make a copy before we start reducing a root method, in case we run
  // into issues, or negative net savings.
  ConcurrentMap<DexMethod*, std::vector<ReducedMethod>>
      concurrent_reduced_methods;
  workqueue_run<std::pair<DexMethod*, InlinableTypes>>(
      [&](const std::pair<DexMethod*, InlinableTypes>& p) {
        auto* method = p.first;
        const auto& types = p.second;
        auto copy_name_str =
            method->get_name()->str() + "$oea$" + std::to_string(types.size());
        auto copy = DexMethod::make_method_from(
            method, method->get_class(), DexString::make_string(copy_name_str));
        RootMethodReducer root_method_reducer{expandable_method_params,
                                              incomplete_marker_method,
                                              inliner,
                                              method_summaries,
                                              stats,
                                              method::is_init(method) ||
                                                  method::is_clinit(method),
                                              copy,
                                              types,
                                              max_inline_size};
        auto reduced_method = root_method_reducer.reduce();
        if (reduced_method) {
          concurrent_reduced_methods.update(
              method, [&](auto*, auto& reduced_methods_variants, bool) {
                reduced_methods_variants.emplace_back(
                    std::move(*reduced_method));
              });
          return;
        }
        DexMethod::erase_method(copy);
        DexMethod::delete_method_DO_NOT_USE(copy);
      },
      ordered_root_methods_variants);

  // For each root method, we order the reduced methods (if any) by how many
  // types where inlined, with the largest number of inlined types going first.
  std::unordered_map<DexMethod*, std::vector<ReducedMethod>> reduced_methods;
  for (auto& [method, reduced_methods_variants] : concurrent_reduced_methods) {
    std::sort(
        reduced_methods_variants.begin(), reduced_methods_variants.end(),
        [&](auto& a, auto& b) { return a.types.size() > b.types.size(); });
    reduced_methods.emplace(method, std::move(reduced_methods_variants));
  }

  // All types which could not be accomodated by any reduced method variants are
  // marked as "irreducible", which is later used when doing a global cost
  // analysis.
  static const InlinableTypes no_types;
  for (auto&& [method, types] : root_methods) {
    auto it = reduced_methods.find(method);
    const auto& largest_types =
        it == reduced_methods.end() ? no_types : it->second.front().types;
    for (auto&& [type, kind] : types) {
      if (!largest_types.count(type)) {
        irreducible_types->insert(type);
      }
    }
  }
  return reduced_methods;
}

// Select all those reduced methods which will result in overall size savings,
// either by looking at local net savings for just a single reduced method, or
// by considering families of reduced methods that affect the same classes.
std::unordered_map<DexMethod*, size_t> select_reduced_methods(
    const std::unordered_map<DexMethod*, std::vector<ReducedMethod>>&
        reduced_methods,
    std::unordered_set<DexType*>* irreducible_types,
    Stats* stats) {
  Timer t("select_reduced_methods");

  // First, we are going to identify all reduced methods which will result in
  // local net savings, considering just a single reduced method at a time.
  // We'll also build up families of reduced methods for which we can later do a
  // global net savings analysis.

  ConcurrentSet<DexType*> concurrent_irreducible_types;
  ConcurrentMap<DexMethod*, size_t> concurrent_selected_reduced_methods;

  // A family of reduced methods for which we'll look at the combined global net
  // savings
  struct Family {
    // Maps root methods to an index into the reduced method variants list.
    std::unordered_map<DexMethod*, size_t> reduced_methods;
    NetSavings global_net_savings;
  };
  ConcurrentMap<DexType*, Family> concurrent_families;
  workqueue_run<std::pair<DexMethod*, std::vector<ReducedMethod>>>(
      [&](const std::pair<DexMethod*, std::vector<ReducedMethod>>& p) {
        auto method = p.first;
        const auto& reduced_methods_variants = p.second;
        always_assert(!reduced_methods_variants.empty());
        auto update_irreducible_types = [&](size_t i) {
          const auto& reduced_method = reduced_methods_variants.at(i);
          const auto& reduced_types = reduced_method.types;
          for (auto&& [type, kind] : reduced_methods_variants.at(0).types) {
            if (!reduced_types.count(type) &&
                kind != InlinableTypeKind::Incomplete) {
              concurrent_irreducible_types.insert(type);
            }
          }
        };
        // We'll try to find the maximal (involving most inlined non-incomplete
        // types) reduced method variant for which we can make the largest
        // non-negative local cost determination.
        boost::optional<std::pair<size_t, int>> best_candidate;
        for (size_t i = 0; i < reduced_methods_variants.size(); i++) {
          // If we couldn't accommodate any types, we'll need to add them to the
          // irreducible types set. Except for the last variant with the least
          // inlined types, for which might below try to make a global cost
          // determination.
          const auto& reduced_method = reduced_methods_variants.at(i);
          auto savings =
              reduced_method.get_net_savings(*irreducible_types).get_value();
          if (!best_candidate || savings > best_candidate->second) {
            best_candidate = std::make_pair(i, savings);
          }
          // If there are no incomplete types left, we can stop here
          const auto& reduced_types = reduced_method.types;
          if (best_candidate && best_candidate->second >= 0 &&
              !std::any_of(reduced_types.begin(), reduced_types.end(),
                           [](auto& p) {
                             return p.second == InlinableTypeKind::Incomplete;
                           })) {
            break;
          }
        }
        if (best_candidate && best_candidate->second >= 0) {
          auto [i, savings] = *best_candidate;
          stats->total_savings += savings;
          concurrent_selected_reduced_methods.emplace(method, i);
          update_irreducible_types(i);
          return;
        }
        update_irreducible_types(reduced_methods_variants.size() - 1);
        const auto& smallest_reduced_method = reduced_methods_variants.back();
        NetSavings conditional_net_savings;
        auto local_net_savings = smallest_reduced_method.get_net_savings(
            *irreducible_types, &conditional_net_savings);
        const auto& classes = conditional_net_savings.classes;
        if (std::any_of(classes.begin(), classes.end(), [&](DexType* type) {
              return irreducible_types->count(type);
            })) {
          stats->too_costly_irreducible_classes++;
        } else if (classes.size() > 1) {
          stats->too_costly_multiple_conditional_classes++;
        } else if (classes.empty()) {
          stats->too_costly_globally++;
        } else {
          always_assert(classes.size() == 1);
          // For a reduced method variant with only a single involved class,
          // we'll do a global cost analysis below.
          auto conditional_type = *classes.begin();
          concurrent_families.update(
              conditional_type, [&](auto*, Family& family, bool) {
                family.global_net_savings += local_net_savings;
                family.global_net_savings += conditional_net_savings;
                family.reduced_methods.emplace(
                    method, reduced_methods_variants.size() - 1);
              });
          return;
        }
        for (auto [type, kind] : smallest_reduced_method.types) {
          concurrent_irreducible_types.insert(type);
        }
      },
      reduced_methods);
  irreducible_types->insert(concurrent_irreducible_types.begin(),
                            concurrent_irreducible_types.end());

  // Second, perform global net savings analysis
  workqueue_run<std::pair<DexType*, Family>>(
      [&](const std::pair<DexType*, Family>& p) {
        auto& [type, family] = p;
        if (irreducible_types->count(type) ||
            family.global_net_savings.get_value() < 0) {
          stats->too_costly_globally += family.reduced_methods.size();
          return;
        }
        stats->total_savings += family.global_net_savings.get_value();
        for (auto& [method, i] : family.reduced_methods) {
          concurrent_selected_reduced_methods.emplace(method, i);
        }
      },
      concurrent_families);

  return std::unordered_map<DexMethod*, size_t>(
      concurrent_selected_reduced_methods.begin(),
      concurrent_selected_reduced_methods.end());
}

void reduce(DexStoresVector& stores,
            const Scope& scope,
            ConfigFiles& conf,
            const init_classes::InitClassesWithSideEffects&
                init_classes_with_side_effects,
            const MethodSummaries& method_summaries,
            const std::unordered_map<DexMethod*, InlinableTypes>& root_methods,
            Stats* stats,
            size_t max_inline_size) {
  Timer t("reduce");
  ConcurrentMethodRefCache concurrent_resolved_refs;
  auto concurrent_resolver = [&](DexMethodRef* method, MethodSearch search) {
    return resolve_method(method, search, concurrent_resolved_refs);
  };

  std::unordered_set<DexMethod*> no_default_inlinables;
  // customize shrinking options
  auto inliner_config = conf.get_inliner_config();
  inliner_config.shrinker = shrinker::ShrinkerConfig();
  inliner_config.shrinker.run_const_prop = true;
  inliner_config.shrinker.run_cse = true;
  inliner_config.shrinker.run_copy_prop = true;
  inliner_config.shrinker.run_local_dce = true;
  inliner_config.shrinker.compute_pure_methods = false;
  int min_sdk = 0;
  MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                             no_default_inlinables, concurrent_resolver,
                             inliner_config, min_sdk,
                             MultiMethodInlinerMode::None);

  // First, we compute all reduced methods

  ExpandableMethodParams expandable_method_params(scope);
  std::unordered_set<DexType*> irreducible_types;
  auto reduced_methods = compute_reduced_methods(
      expandable_method_params, inliner, method_summaries, root_methods,
      &irreducible_types, stats, max_inline_size);
  stats->reduced_methods = reduced_methods.size();

  // Second, we select reduced methods that will result in net savings
  auto selected_reduced_methods =
      select_reduced_methods(reduced_methods, &irreducible_types, stats);
  stats->selected_reduced_methods = selected_reduced_methods.size();

  // Finally, we are going to apply those selected methods, and clean up all
  // reduced method clones
  workqueue_run<std::pair<DexMethod*, std::vector<ReducedMethod>>>(
      [&](const std::pair<DexMethod*, std::vector<ReducedMethod>>& p) {
        auto& [method, reduced_methods_variants] = p;
        auto it = selected_reduced_methods.find(method);
        if (it != selected_reduced_methods.end()) {
          auto& reduced_method = reduced_methods_variants.at(it->second);
          method->set_code(reduced_method.method->release_code());
          stats->calls_inlined += reduced_method.calls_inlined;
          stats->new_instances_eliminated +=
              reduced_method.new_instances_eliminated;
        }
        stats->reduced_methods_variants += reduced_methods_variants.size();
        for (auto& reduced_method : reduced_methods_variants) {
          DexMethod::erase_method(reduced_method.method);
          DexMethod::delete_method_DO_NOT_USE(reduced_method.method);
        }
      },
      reduced_methods);

  size_t expanded_methods = expandable_method_params.flush(scope);
  stats->expanded_methods += expanded_methods;
}
} // namespace

void ObjectEscapeAnalysisPass::bind_config() {
  bind("max_inline_size", MAX_INLINE_SIZE, m_max_inline_size);
}

void ObjectEscapeAnalysisPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& conf,
                                        PassManager& mgr) {
  const auto scope = build_class_scope(stores);
  auto method_override_graph = mog::build_graph(scope);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, conf.create_init_class_insns(), method_override_graph.get());
  auto non_overridden_virtuals =
      hier::find_non_overridden_virtuals(*method_override_graph);

  std::unordered_map<DexType*, Locations> new_instances;
  std::unordered_map<DexMethod*, Locations> invokes;
  std::unordered_map<DexMethod*, std::unordered_set<DexMethod*>> dependencies;
  analyze_scope(scope, non_overridden_virtuals, &new_instances, &invokes,
                &dependencies);

  auto method_summaries = compute_method_summaries(mgr, scope, dependencies,
                                                   non_overridden_virtuals);

  auto inline_anchors = compute_inline_anchors(scope, method_summaries);

  auto root_methods = compute_root_methods(mgr, new_instances, invokes,
                                           method_summaries, inline_anchors);

  Stats stats;
  reduce(stores, scope, conf, init_classes_with_side_effects, method_summaries,
         root_methods, &stats, m_max_inline_size);

  walk::parallel::code(scope,
                       [&](DexMethod*, IRCode& code) { code.clear_cfg(); });

  TRACE(OEA, 1, "[object escape analysis] total savings: %zu",
        (size_t)stats.total_savings);
  TRACE(
      OEA, 1,
      "[object escape analysis] %zu root methods lead to %zu reduced root "
      "methods with %zu variants "
      "of which %zu were selected and %zu anchors not inlinable because "
      "inlining failed, %zu/%zu invokes not inlinable because callee is "
      "init, %zu invokes not inlinable because callee is not concrete,"
      "%zu invokes not inlinable because inlining failed, %zu invokes not "
      "inlinable after too many iterations, %zu stackify returned objects, "
      "%zu too costly with irreducible classes, %zu too costly with multiple "
      "conditional classes, %zu too costly globally; %zu expanded methods; %zu "
      "calls inlined; %zu new-instances eliminated",
      root_methods.size(), (size_t)stats.reduced_methods,
      (size_t)stats.reduced_methods_variants,
      (size_t)stats.selected_reduced_methods,
      (size_t)stats.anchors_not_inlinable_inlining,
      (size_t)stats.invokes_not_inlinable_callee_unexpandable,
      (size_t)stats.invokes_not_inlinable_callee_too_many_params_to_expand,
      (size_t)stats.invokes_not_inlinable_callee_inconcrete,
      (size_t)stats.invokes_not_inlinable_inlining,
      (size_t)stats.invokes_not_inlinable_too_many_iterations,
      (size_t)stats.stackify_returns_objects,
      (size_t)stats.too_costly_irreducible_classes,
      (size_t)stats.too_costly_multiple_conditional_classes,
      (size_t)stats.too_costly_globally, (size_t)stats.expanded_methods,
      (size_t)stats.calls_inlined, (size_t)stats.new_instances_eliminated);

  mgr.incr_metric("total_savings", stats.total_savings);
  mgr.incr_metric("root_methods", root_methods.size());
  mgr.incr_metric("reduced_methods", (size_t)stats.reduced_methods);
  mgr.incr_metric("reduced_methods_variants",
                  (size_t)stats.reduced_methods_variants);
  mgr.incr_metric("selected_reduced_methods",
                  (size_t)stats.selected_reduced_methods);
  mgr.incr_metric("root_method_anchors_not_inlinable_inlining",
                  (size_t)stats.anchors_not_inlinable_inlining);
  mgr.incr_metric("root_method_invokes_not_inlinable_callee_unexpandable",
                  (size_t)stats.invokes_not_inlinable_callee_unexpandable);
  mgr.incr_metric(
      "root_method_invokes_not_inlinable_callee_is_init_too_many_params_to_"
      "expand",
      (size_t)stats.invokes_not_inlinable_callee_too_many_params_to_expand);
  mgr.incr_metric("root_method_invokes_not_inlinable_callee_inconcrete",
                  (size_t)stats.invokes_not_inlinable_callee_inconcrete);
  mgr.incr_metric("root_method_invokes_not_inlinable_inlining",
                  (size_t)stats.invokes_not_inlinable_inlining);
  mgr.incr_metric("root_method_invokes_not_inlinable_too_many_iterations",
                  (size_t)stats.invokes_not_inlinable_too_many_iterations);
  mgr.incr_metric("root_method_stackify_returns_objects",
                  (size_t)stats.stackify_returns_objects);
  mgr.incr_metric("root_method_too_costly_globally",
                  (size_t)stats.too_costly_globally);
  mgr.incr_metric("root_method_too_costly_multiple_conditional_classes",
                  (size_t)stats.too_costly_multiple_conditional_classes);
  mgr.incr_metric("root_method_too_costly_irreducible_classes",
                  (size_t)stats.too_costly_irreducible_classes);
  mgr.incr_metric("expanded_methods", (size_t)stats.expanded_methods);
  mgr.incr_metric("calls_inlined", (size_t)stats.calls_inlined);
  mgr.incr_metric("new_instances_eliminated",
                  (size_t)stats.new_instances_eliminated);
}

static ObjectEscapeAnalysisPass s_pass;
