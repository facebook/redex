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
 * - Refine the net-savings computation to not just make decisions per
 *   root-method, but across all root-methods
 * - Refine the tracing of object allocations in root methods to ignore
 *   unanchored object allocations
 * - Instead of inlining all invoked methods, consider transforming those which
 *   do not mutate or compare the allocated object as follows: instead of
 *   passing in the allocated object via an argument, pass in all read fields
 *   are passed in as separate arguments. This could reduce the size increase
 *   due to multiple inlined method body copies, and it could enable continuing
 *   when the allocated object is passed into another constructor, where we
 *   currently give up.
 */

#include "ObjectEscapeAnalysis.h"

#include <algorithm>
#include <optional>

#include "BaseIRAnalyzer.h"
#include "CFGMutation.h"
#include "ConfigFiles.h"
#include "ConstantAbstractDomain.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "Inliner.h"
#include "LiveRange.h"
#include "MethodOverrideGraph.h"
#include "PassManager.h"
#include "PatriciaTreeMap.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "PatriciaTreeSet.h"
#include "PatriciaTreeSetAbstractDomain.h"
#include "Resolver.h"
#include "Show.h"
#include "Walkers.h"

using namespace sparta;
namespace mog = method_override_graph;

namespace {

const int MAX_INLINE_INVOKES_ITERATIONS = 8;

using Locations = std::vector<std::pair<DexMethod*, const IRInstruction*>>;
// Collect all allocation and invoke instructions, as well as non-virtual
// invocation dependencies.
void analyze_scope(
    const Scope& scope,
    const std::unordered_set<DexMethod*>& non_true_virtual,
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
            (!callee->is_virtual() || non_true_virtual.count(callee))) {
          concurrent_invokes.update(callee, [&](auto*, auto& vec, bool) {
            vec.emplace_back(method, insn);
          });
          if (!method->is_virtual() || non_true_virtual.count(method)) {
            concurrent_dependencies.update(
                callee,
                [method](auto, auto& set, auto) { set.insert(method); });
          }
        }
      }
    }
  });
  for (auto& p : concurrent_new_instances) {
    new_instances->insert(std::move(p));
  }
  for (auto& p : concurrent_invokes) {
    invokes->insert(std::move(p));
  }
  for (auto& p : concurrent_dependencies) {
    dependencies->insert(std::move(p));
  }
}

// A benign method invocation can be ignored during the escape analysis.
bool is_benign(const DexMethodRef* method_ref) {
  static const std::unordered_set<std::string> methods = {
      // clang-format off
      "Ljava/lang/Object;.<init>:()V",
      // clang-format on
  };

  return method_ref->is_def() &&
         methods.count(method_ref->as_def()->get_deobfuscated_name_or_empty());
}

constexpr const IRInstruction* NO_ALLOCATION = nullptr;

using namespace ir_analyzer;

// For each object, we track which instruction might have allocated it:
// - new-instance and invoke- instruction might represent allocation points
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

// The analyzer computes...
// - which instructions allocate (new-instance, invoke-)
// - which allocations escape (and how)
// - which allocations return
class Analyzer final : public BaseIRAnalyzer<Environment> {
 public:
  explicit Analyzer(
      const std::unordered_map<DexMethod*, MethodSummary>& method_summaries,
      cfg::ControlFlowGraph& cfg)
      : BaseIRAnalyzer(cfg), m_method_summaries(method_summaries) {
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
      if (is_benign(insn->get_method())) {
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

  const std::unordered_map<const IRInstruction*,
                           std::unordered_set<live_range::Use>>&
  get_escapes() {
    return m_escapes;
  }

  const std::unordered_set<const IRInstruction*>& get_returns() {
    return m_returns;
  }

  std::unordered_set<IRInstruction*> get_inlinables() {
    std::unordered_set<IRInstruction*> inlinables;
    for (auto& p : m_escapes) {
      if (p.second.empty() && p.first->opcode() != IOPCODE_LOAD_PARAM_OBJECT &&
          !m_returns.count(p.first)) {
        inlinables.insert(const_cast<IRInstruction*>(p.first));
      }
    }
    return inlinables;
  }

 private:
  const std::unordered_map<DexMethod*, MethodSummary>& m_method_summaries;
  mutable std::unordered_map<const IRInstruction*,
                             std::unordered_set<live_range::Use>>
      m_escapes;
  mutable std::unordered_set<const IRInstruction*> m_returns;
};

std::unordered_map<DexMethod*, MethodSummary> compute_method_summaries(
    PassManager& mgr,
    const Scope& scope,
    const std::unordered_map<DexMethod*, std::unordered_set<DexMethod*>>&
        dependencies,
    const std::unordered_set<DexMethod*>& non_true_virtual) {
  Timer t("compute_method_summaries");

  std::unordered_set<DexMethod*> impacted_methods;
  walk::code(scope, [&](DexMethod* method, IRCode&) {
    if (!method->is_virtual() || non_true_virtual.count(method)) {
      impacted_methods.insert(method);
    }
  });

  std::unordered_map<DexMethod*, MethodSummary> method_summaries;
  size_t analysis_iterations = 0;
  while (!impacted_methods.empty()) {
    Timer t2("analysis iteration");
    analysis_iterations++;
    TRACE(OEA, 1, "[object escape analysis] analysis_iteration %zu",
          analysis_iterations);
    ConcurrentMap<DexMethod*, MethodSummary> recomputed_method_summaries;
    workqueue_run<DexMethod*>(
        [&](DexMethod* method) {
          auto& cfg = method->get_code()->cfg();
          Analyzer analyzer(method_summaries, cfg);
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
    for (auto& p : recomputed_method_summaries) {
      auto& ms = method_summaries[p.first];
      for (auto src_index : p.second.benign_params) {
        if (ms.benign_params.insert(src_index).second) {
          changed_methods.insert(p.first);
        }
      }
      if (p.second.allocation_insn && !ms.allocation_insn) {
        ms.allocation_insn = p.second.allocation_insn;
        changed_methods.insert(p.first);
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

DexType* get_allocated_type(
    const std::unordered_map<DexMethod*, MethodSummary>& method_summaries,
    DexMethod* method) {
  auto insn = method_summaries.at(method).allocation_insn;
  while (insn->opcode() != OPCODE_NEW_INSTANCE) {
    always_assert(opcode::is_an_invoke(insn->opcode()));
    auto callee = resolve_method(insn->get_method(), opcode_to_search(insn));
    insn = method_summaries.at(callee).allocation_insn;
  }
  return insn->get_type();
}

using InlineAnchorsOfType =
    std::unordered_map<DexMethod*, std::unordered_set<IRInstruction*>>;
std::unordered_map<DexType*, InlineAnchorsOfType> compute_inline_anchors(
    const Scope& scope,
    const std::unordered_map<DexMethod*, MethodSummary>& method_summaries) {
  Timer t("compute_inline_anchors");
  ConcurrentMap<DexType*, InlineAnchorsOfType> concurrent_inline_anchors;
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    Analyzer analyzer(method_summaries, code.cfg());
    auto inlinables = analyzer.get_inlinables();
    for (auto insn : inlinables) {
      if (insn->opcode() == OPCODE_NEW_INSTANCE) {
        TRACE(OEA, 3, "[object escape analysis] inline anchor [%s] %s",
              SHOW(method), SHOW(insn));
        concurrent_inline_anchors.update(
            insn->get_type(),
            [&](auto*, auto& map, bool) { map[method].insert(insn); });
        continue;
      }
      always_assert(opcode::is_an_invoke(insn->opcode()));
      auto callee = resolve_method(insn->get_method(), opcode_to_search(insn));
      always_assert(method_summaries.at(callee).allocation_insn);
      auto type = get_allocated_type(method_summaries, callee);
      TRACE(OEA, 3, "[object escape analysis] inline anchor [%s] %s",
            SHOW(method), SHOW(insn));
      concurrent_inline_anchors.update(
          type, [&](auto*, auto& map, bool) { map[method].insert(insn); });
    }
  });
  std::unordered_map<DexType*, InlineAnchorsOfType> inline_anchors;
  for (auto& p : concurrent_inline_anchors) {
    inline_anchors.insert(std::move(p));
  }
  return inline_anchors;
}

std::unordered_map<DexMethod*, std::unordered_map<DexType*, bool>>
compute_root_methods(
    PassManager& mgr,
    const std::unordered_map<DexType*, Locations>& new_instances,
    const std::unordered_map<DexMethod*, Locations>& invokes,
    const std::unordered_map<DexMethod*, MethodSummary>& method_summaries,
    const std::unordered_map<DexType*, InlineAnchorsOfType>& inline_anchors) {
  Timer t("compute_root_methods");
  std::unordered_set<DexType*> candidate_types;
  std::unordered_map<DexMethod*, std::unordered_map<DexType*, bool>>
      root_methods;
  for (auto& [type, method_insn_pairs] : new_instances) {
    auto it = inline_anchors.find(type);
    if (it == inline_anchors.end()) {
      continue;
    }
    auto& inline_anchors_of_type = it->second;
    bool multiples = inline_anchors_of_type.size() > 1;

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
      if (it4->second.size() > 1) {
        multiples = true;
      }
      for (auto q : it4->second) {
        if (!is_anchored(q)) {
          return false;
        }
      }
      return true;
    };

    if (std::all_of(method_insn_pairs.begin(), method_insn_pairs.end(),
                    is_anchored)) {
      candidate_types.insert(type);
      for (auto& [method, insns] : inline_anchors_of_type) {
        if (!method->rstate.no_optimizations()) {
          TRACE(OEA, 3, "[object escape analysis] root method %s with %s%s",
                SHOW(method), SHOW(type), multiples ? " multiples" : "");
          root_methods[method].emplace(type, multiples);
        }
      }
    }
  }

  TRACE(OEA, 1, "[object escape analysis] candidate types: %zu",
        candidate_types.size());
  mgr.incr_metric("candidate types", candidate_types.size());
  return root_methods;
}

size_t get_code_size(DexMethod* method) {
  auto& cfg = method->get_code()->cfg();
  size_t code_size{0};
  for (auto& mie : InstructionIterable(cfg)) {
    auto insn = mie.insn;
    if (opcode::is_a_move(insn->opcode()) ||
        opcode::is_a_return(insn->opcode())) {
      continue;
    }
    code_size += mie.insn->size();
  }
  return code_size;
}

struct Stats {
  std::atomic<size_t> total_savings{0};
  std::atomic<size_t> reduced_methods{0};
  std::atomic<size_t> invokes_not_inlinable_callee_is_init{0};
  std::atomic<size_t> invokes_not_inlinable_inlining{0};
  std::atomic<size_t> invokes_not_inlinable_too_many_iterations{0};
  std::atomic<size_t> anchors_not_inlinable_inlining{0};
  std::atomic<size_t> stackify_returns_objects{0};
  std::atomic<size_t> too_costly{0};
};

struct ReducedMethod {
  DexMethod* method;
  size_t initial_code_size;
  std::unordered_map<DexMethod*, std::unordered_set<DexType*>> inlined_methods;

  int get_net_savings(const std::unordered_map<DexType*, bool>& types) const {
    auto final_code_size = get_code_size(method);
    int net_savings = initial_code_size - final_code_size;

    std::unordered_set<DexType*> remaining;
    for (auto& [inlined_method, inlined_types] : inlined_methods) {
      auto code_size = get_code_size(inlined_method);
      net_savings += 4 + (inlined_method->get_proto()->is_void() ? 0 : 3);
      bool any_remaining = false;
      for (auto t : inlined_types) {
        if (types.at(t) || !can_delete(inlined_method)) {
          remaining.insert(t);
          any_remaining = true;
        }
      }
      if (!any_remaining) {
        net_savings += 16 + code_size;
      }
    }

    for (auto [type, multiples] : types) {
      if (remaining.count(type) || multiples) {
        continue;
      }
      auto cls = type_class(type);
      always_assert(cls);
      if (can_delete(cls) && !cls->get_clinit()) {
        net_savings += 48;
      }
      for (auto field : cls->get_ifields()) {
        if (can_delete(field)) {
          net_savings += 8;
        }
      }
    }
    return net_savings;
  }
};

class RootMethodReducer {
 private:
  MultiMethodInliner& m_inliner;
  const std::unordered_map<DexMethod*, MethodSummary>& m_method_summaries;
  Stats* m_stats;
  bool m_is_init_or_clinit;
  DexMethod* m_method;
  const std::unordered_map<DexType*, bool>& m_types;

 public:
  RootMethodReducer(
      MultiMethodInliner& inliner,
      const std::unordered_map<DexMethod*, MethodSummary>& method_summaries,
      Stats* stats,
      bool is_init_or_clinit,
      DexMethod* method,
      const std::unordered_map<DexType*, bool>& types)
      : m_inliner(inliner),
        m_method_summaries(method_summaries),
        m_stats(stats),
        m_is_init_or_clinit(is_init_or_clinit),
        m_method(method),
        m_types(types) {}

  std::optional<ReducedMethod> reduce() {
    shrink();
    auto initial_code_size{get_code_size(m_method)};

    if (!inline_anchors() || !inline_invokes()) {
      return std::nullopt;
    }

    while (auto* insn = find_inlinable_new_instance()) {
      if (!stackify(insn)) {
        return std::nullopt;
      }
    }

    shrink();
    return (ReducedMethod){m_method, initial_code_size,
                           std::move(m_inlined_methods)};
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
    return inlined == insns.size();
  }

  // Inline all "anchors" until all relevant allocations are new-instance
  // instructions in the (root) method.
  bool inline_anchors() {
    auto& cfg = m_method->get_code()->cfg();
    while (true) {
      Analyzer analyzer(m_method_summaries, cfg);
      std::unordered_set<IRInstruction*> invokes_to_inline;
      auto inlinables = analyzer.get_inlinables();
      for (auto insn : inlinables) {
        if (insn->opcode() == OPCODE_NEW_INSTANCE) {
          continue;
        }
        always_assert(opcode::is_an_invoke(insn->opcode()));
        auto callee =
            resolve_method(insn->get_method(), opcode_to_search(insn));
        auto type = get_allocated_type(m_method_summaries, callee);
        if (m_types.count(type)) {
          invokes_to_inline.insert(insn);
          m_inlined_methods[callee].insert(type);
        }
      }
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

  IRInstruction* find_inlinable_new_instance() const {
    auto& cfg = m_method->get_code()->cfg();
    for (auto& mie : InstructionIterable(cfg)) {
      auto insn = mie.insn;
      if (is_inlinable_new_instance(insn)) {
        return insn;
      }
    }
    return nullptr;
  }

  // Inline all uses of all relevant new-instance instructions that involve
  // invoke- instructions, until there are no more such uses.
  bool inline_invokes() {
    auto& cfg = m_method->get_code()->cfg();
    for (int iteration = 0; iteration < MAX_INLINE_INVOKES_ITERATIONS;
         iteration++) {
      std::unordered_set<IRInstruction*> invokes_to_inline;

      live_range::MoveAwareChains chains(cfg);
      auto du_chains = chains.get_def_use_chains();
      for (auto& [insn, uses] : du_chains) {
        if (!is_inlinable_new_instance(insn)) {
          continue;
        }
        std::unordered_map<IRInstruction*, bool> aggregated_uses;
        for (auto& use : uses) {
          aggregated_uses[use.insn] |= (use.src_index == 0);
        }
        for (auto& [uses_insn, uses_src_index_zero] : aggregated_uses) {
          if (opcode::is_an_invoke(uses_insn->opcode())) {
            if (!is_benign(uses_insn->get_method())) {
              auto callee = resolve_method(uses_insn->get_method(),
                                           opcode_to_search(uses_insn));
              always_assert(callee);
              if (method::is_init(callee) && !uses_src_index_zero) {
                m_stats->invokes_not_inlinable_callee_is_init++;
                return false;
              }

              invokes_to_inline.insert(uses_insn);
              m_inlined_methods[callee].insert(insn->get_type());
            }
          }
        }
      }

      if (invokes_to_inline.empty()) {
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
  bool stackify(IRInstruction* new_instance_insn) {
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

    live_range::MoveAwareChains chains(cfg);
    auto du_chains = chains.get_def_use_chains();
    auto uses = du_chains[new_instance_insn];
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
        always_assert(is_benign(insn->get_method()));
        if (!identity_matters) {
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
    return true;
  }

  std::unordered_map<DexMethod*, std::unordered_set<DexType*>>
      m_inlined_methods;
};

void reduce(
    DexStoresVector& stores,
    const Scope& scope,
    ConfigFiles& conf,
    const std::unordered_map<DexMethod*, MethodSummary>& method_summaries,
    const std::unordered_map<DexMethod*, std::unordered_map<DexType*, bool>>&
        root_methods,
    Stats* stats) {
  Timer t("reduce");
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, conf.create_init_class_insns());

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

  // We make a copy before we start reducing a root method, in case we run
  // into issues, or negative net savings.
  ConcurrentMap<DexMethod*, ReducedMethod> reduced_methods;
  workqueue_run<std::pair<DexMethod*, std::unordered_map<DexType*, bool>>>(
      [&](const std::pair<DexMethod*, std::unordered_map<DexType*, bool>>& p) {
        auto& [method, types] = p;
        const std::string& name_str = method->get_name()->str();
        DexMethod* copy = DexMethod::make_method_from(
            method,
            method->get_class(),
            DexString::make_string(name_str + "$redex_stack_allocated"));
        RootMethodReducer root_method_reducer{
            inliner, method_summaries,
            stats,   method::is_init(method) || method::is_clinit(method),
            copy,    types};
        auto reduced_method = root_method_reducer.reduce();
        if (!reduced_method) {
          DexMethod::erase_method(copy);
          DexMethod::delete_method_DO_NOT_USE(copy);
          return;
        }

        reduced_methods.emplace(method, std::move(*reduced_method));
      },
      root_methods);

  workqueue_run<std::pair<DexMethod*, ReducedMethod>>(
      [&](const std::pair<DexMethod*, ReducedMethod>& p) {
        auto& [method, reduced_method] = p;
        auto& types = root_methods.at(method);

        auto net_savings = reduced_method.get_net_savings(types);
        if (net_savings >= 0) {
          stats->total_savings += net_savings;
          method->set_code(reduced_method.method->release_code());
        } else {
          stats->too_costly++;
        }

        DexMethod::erase_method(reduced_method.method);
        DexMethod::delete_method_DO_NOT_USE(reduced_method.method);
      },
      reduced_methods);

  stats->reduced_methods = reduced_methods.size();
}
} // namespace

void ObjectEscapeAnalysisPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& conf,
                                        PassManager& mgr) {
  const auto scope = build_class_scope(stores);
  auto method_override_graph = mog::build_graph(scope);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, conf.create_init_class_insns(), method_override_graph.get());
  auto non_true_virtual =
      mog::get_non_true_virtuals(*method_override_graph, scope);

  std::unordered_map<DexType*, Locations> new_instances;
  std::unordered_map<DexMethod*, Locations> invokes;
  std::unordered_map<DexMethod*, std::unordered_set<DexMethod*>> dependencies;
  analyze_scope(scope, non_true_virtual, &new_instances, &invokes,
                &dependencies);

  auto method_summaries =
      compute_method_summaries(mgr, scope, dependencies, non_true_virtual);

  auto inline_anchors = compute_inline_anchors(scope, method_summaries);

  auto root_methods = compute_root_methods(mgr, new_instances, invokes,
                                           method_summaries, inline_anchors);

  Stats stats;
  reduce(stores, scope, conf, method_summaries, root_methods, &stats);

  walk::parallel::code(scope,
                       [&](DexMethod*, IRCode& code) { code.clear_cfg(); });

  TRACE(OEA, 1, "[object escape analysis] total savings: %zu",
        (size_t)stats.total_savings);
  TRACE(
      OEA, 1,
      "[object escape analysis] %zu root methods lead to %zu reduced methods "
      "and %zu anchors not inlinable because inlining failed, %zu invokes not "
      "inlinable because callee is init, %zu invokes not inlinable because "
      "inlining failed, %zu invokes not inlinable after too many iterations, "
      "%zu stackify returned objects, %zu too costly",
      root_methods.size(), (size_t)stats.reduced_methods,
      (size_t)stats.anchors_not_inlinable_inlining,
      (size_t)stats.invokes_not_inlinable_callee_is_init,
      (size_t)stats.invokes_not_inlinable_inlining,
      (size_t)stats.invokes_not_inlinable_too_many_iterations,
      (size_t)stats.stackify_returns_objects, (size_t)stats.too_costly);

  mgr.incr_metric("total_savings", stats.total_savings);
  mgr.incr_metric("root_methods", root_methods.size());
  mgr.incr_metric("reduced_methods", (size_t)stats.reduced_methods);
  mgr.incr_metric("root_method_anchors_not_inlinable_inlining",
                  (size_t)stats.anchors_not_inlinable_inlining);
  mgr.incr_metric("root_method_invokes_not_inlinable_callee_is_init",
                  (size_t)stats.invokes_not_inlinable_callee_is_init);
  mgr.incr_metric("root_method_invokes_not_inlinable_inlining",
                  (size_t)stats.invokes_not_inlinable_inlining);
  mgr.incr_metric("root_method_invokes_not_inlinable_too_many_iterations",
                  (size_t)stats.invokes_not_inlinable_too_many_iterations);
  mgr.incr_metric("root_method_stackify_returns_objects",
                  (size_t)stats.stackify_returns_objects);
  mgr.incr_metric("root_method_too_costly", (size_t)stats.too_costly);
}

static ObjectEscapeAnalysisPass s_pass;
