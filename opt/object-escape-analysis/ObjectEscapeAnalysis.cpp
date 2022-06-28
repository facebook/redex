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
#include "HierarchyUtil.h"
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
#include "StringBuilder.h"
#include "Walkers.h"

using namespace sparta;
namespace mog = method_override_graph;
namespace hier = hierarchy_util;

namespace {

const int MAX_INLINE_INVOKES_ITERATIONS = 8;

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
  mutable Escapes m_escapes;
  mutable std::unordered_set<const IRInstruction*> m_returns;
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
    Analyzer analyzer(method_summaries, code.cfg());
    auto inlinables = analyzer.get_inlinables();
    for (auto insn : inlinables) {
      auto [callee, type] = resolve_inlinable(method_summaries, insn);
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

// Maps types to a bool indicating whether there are multiple uses.
using InlinableTypes = std::unordered_map<DexType*, bool>;

std::unordered_map<DexMethod*, InlinableTypes> compute_root_methods(
    PassManager& mgr,
    const std::unordered_map<DexType*, Locations>& new_instances,
    const std::unordered_map<DexMethod*, Locations>& invokes,
    const MethodSummaries& method_summaries,
    const std::unordered_map<DexType*, InlineAnchorsOfType>& inline_anchors) {
  Timer t("compute_root_methods");
  std::unordered_set<DexType*> candidate_types;
  std::unordered_map<DexMethod*, InlinableTypes> root_methods;
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
  std::atomic<size_t> reduced_methods_variants{0};
  std::atomic<size_t> selected_reduced_methods{0};
  std::atomic<size_t> invokes_not_inlinable_callee_is_init_unexpandable{0};
  std::atomic<size_t> invokes_not_inlinable_callee_inconcrete{0};
  std::atomic<size_t>
      invokes_not_inlinable_callee_is_init_too_many_params_to_expand{0};
  std::atomic<size_t> invokes_not_inlinable_inlining{0};
  std::atomic<size_t> invokes_not_inlinable_too_many_iterations{0};
  std::atomic<size_t> anchors_not_inlinable_inlining{0};
  std::atomic<size_t> stackify_returns_objects{0};
  std::atomic<size_t> too_costly_multiple_conditional_classes{0};
  std::atomic<size_t> too_costly_irreducible_classes{0};
  std::atomic<size_t> too_costly_globally{0};
  std::atomic<size_t> expanded_ctors{0};
};

// Predict what a method's deobfuscated name would be.
std::string show_deobfuscated(const DexType* type,
                              const DexString* name,
                              const DexProto* proto) {
  string_builders::StaticStringBuilder<5> b;
  b << show_deobfuscated(type) << "." << show(name) << ":"
    << show_deobfuscated(proto);
  return b.str();
}

// Helper class to deal with (otherwise uninlinable) constructors that take a
// (newly created) object, and only use it to read ifields. For those
// constructors, we identify when we can replace the (newly created) object
// parameter with a sequence of field value parameters.
class ExpandableConstructorParams {
 private:
  // For each class, and each constructor, and each parameter, we record the
  // (ordered) list of ifields that are read from the parameter, if the
  // parameter doesn't otherwise escape, and the implied expanded constructor
  // arg list is not in conflict with any other constructor arg list.
  using ClassInfo = std::unordered_map<
      DexMethod*,
      std::unordered_map<param_index_t, std::vector<DexField*>>>;
  mutable ConcurrentMap<DexType*, std::shared_ptr<ClassInfo>> m_class_infos;
  // For each requested expanded constructor method ref, we remember the
  // original and ctor, and which parameter was expanded.
  using MethodParam = std::pair<DexMethod*, param_index_t>;
  mutable std::unordered_map<DexMethodRef*, MethodParam> m_candidates;
  mutable std::mutex m_candidates_mutex;
  // We keep track of deobfuscated ctor names already in use before the pass, to
  // avoid reusing them.
  std::unordered_set<const DexString*> m_deobfuscated_ctor_names;

  static std::vector<DexType*> get_expanded_args_vector(
      DexMethod* ctor,
      param_index_t param_index,
      const std::vector<DexField*>& fields) {
    always_assert(param_index > 0);
    auto args = ctor->get_proto()->get_args();
    always_assert(param_index <= args->size());
    std::vector<DexType*> args_vector;
    args_vector.reserve(args->size() - 1 + fields.size());
    for (param_index_t i = 0; i < args->size(); i++) {
      if (i != param_index - 1) {
        args_vector.push_back(args->at(i));
        continue;
      }
      for (auto f : fields) {
        args_vector.push_back(f->get_type());
      }
    }
    return args_vector;
  }

  // Get or create the class-info for a given type.
  ClassInfo* get_class_info(DexType* type) const {
    auto res = m_class_infos.get(type, nullptr);
    if (res) {
      return res.get();
    }
    res = std::make_shared<ClassInfo>();
    std::set<std::vector<DexType*>> args_vectors;
    auto cls = type_class(type);
    if (cls) {
      // First, collect all of the (guaranteed to be distinct) args of the
      // existing constructors.
      for (auto* ctor : cls->get_ctors()) {
        auto args = ctor->get_proto()->get_args();
        std::vector<DexType*> args_vector(args->begin(), args->end());
        auto inserted = args_vectors.insert(std::move(args_vector)).second;
        always_assert(inserted);
      }
      // Second, for each ctor, and each (non-first) parameter that is only used
      // in igets, compute the expanded constructor args and record them if they
      // don't create a conflict.
      for (auto* ctor : cls->get_ctors()) {
        auto code = ctor->get_code();
        if (!code || ctor->rstate.no_optimizations()) {
          continue;
        }
        live_range::MoveAwareChains chains(code->cfg());
        auto du_chains = chains.get_def_use_chains();
        param_index_t param_index{1};
        auto ii = code->cfg().get_param_instructions();
        for (auto it = std::next(ii.begin()); it != ii.end();
             it++, param_index++) {
          bool expandable{true};
          std::vector<DexField*> fields;
          for (auto& use : du_chains[it->insn]) {
            if (opcode::is_an_iget(use.insn->opcode())) {
              auto* field =
                  resolve_field(use.insn->get_field(), FieldSearch::Instance);
              if (field) {
                fields.push_back(field);
                continue;
              }
            }
            expandable = false;
            break;
          }
          if (!expandable) {
            continue;
          }
          std::sort(fields.begin(), fields.end(), compare_dexfields);
          // remove duplicates
          fields.erase(std::unique(fields.begin(), fields.end()), fields.end());
          auto expanded_args_vector =
              get_expanded_args_vector(ctor, param_index, fields);
          // We need to check if we don't have too many args that won't fit into
          // an invoke/range instruction.
          uint32_t range_size = 1;
          for (auto arg_type : expanded_args_vector) {
            range_size += type::is_wide_type(arg_type) ? 2 : 1;
          }
          if (range_size <= 0xff) {
            auto inserted =
                args_vectors.insert(std::move(expanded_args_vector)).second;
            if (inserted) {
              (*res)[ctor].emplace(param_index, std::move(fields));
            }
          }
        }
      }
    }
    m_class_infos.update(type, [&](auto*, auto& value, bool exists) {
      if (exists) {
        // Oh well, we wasted some racing with another thread.
        res = value;
        return;
      }
      value = res;
    });
    return res.get();
  }

  // Given an earlier created expanded constructor method ref, fill in the code.
  DexMethod* make_expanded_ctor_concrete(DexMethodRef* expanded_ctor_ref) {
    auto [ctor, param_index] = m_candidates.at(expanded_ctor_ref);

    // We start from the original ctor method body, and mutate a copy.
    std::unique_ptr<IRCode> cloned_code =
        std::make_unique<IRCode>(std::make_unique<cfg::ControlFlowGraph>());
    ctor->get_code()->cfg().deep_copy(&cloned_code->cfg());
    auto& cfg = cloned_code->cfg();
    cfg::CFGMutation mutation(cfg);

    // Replace load-param of (newly created) object with a sequence of
    // load-params for the field values used by the ctor; initialize the (newly
    // created) object register with a const-0, so that any remaining
    // move-object instructions are still valid.
    auto block = cfg.entry_block();
    auto load_param_it =
        block->to_cfg_instruction_iterator(block->get_first_insn());
    always_assert(!load_param_it.is_end());
    for (param_index_t i = 0; i < param_index; i++) {
      load_param_it++;
      always_assert(!load_param_it.is_end());
    }
    auto last_load_params_it = block->to_cfg_instruction_iterator(
        block->get_last_param_loading_insn());
    auto null_insn = (new IRInstruction(OPCODE_CONST))
                         ->set_dest(load_param_it->insn->dest())
                         ->set_literal(0);
    mutation.insert_after(last_load_params_it, {null_insn});

    std::vector<IRInstruction*> new_load_param_insns;
    std::unordered_map<DexField*, reg_t> field_regs;
    auto& fields =
        m_class_infos.at_unsafe(ctor->get_class())->at(ctor).at(param_index);
    for (auto field : fields) {
      auto reg = type::is_wide_type(field->get_type())
                     ? cfg.allocate_wide_temp()
                     : cfg.allocate_temp();
      auto inserted = field_regs.emplace(field, reg).second;
      always_assert(inserted);
      auto load_param_insn =
          (new IRInstruction(opcode::load_opcode(field->get_type())))
              ->set_dest(reg);
      new_load_param_insns.push_back(load_param_insn);
    }
    mutation.replace(load_param_it, new_load_param_insns);

    // Replace all igets on the (newly created) object with moves from the new
    // field value load-params. No other (non-move) uses of the (newly created)
    // object can exist.
    live_range::MoveAwareChains chains(cfg);
    auto du_chains = chains.get_def_use_chains();
    std::unordered_set<IRInstruction*> use_insns;
    for (auto& use : du_chains[load_param_it->insn]) {
      use_insns.insert(use.insn);
    }
    auto ii = InstructionIterable(cfg);
    for (auto it = ii.begin(); it != ii.end(); it++) {
      if (!use_insns.count(it->insn)) {
        continue;
      }
      auto insn = it->insn;
      always_assert(opcode::is_an_iget(insn->opcode()));
      auto* field = resolve_field(insn->get_field(), FieldSearch::Instance);
      always_assert(field);
      auto move_result_pseudo_it = cfg.move_result_of(it);
      always_assert(!move_result_pseudo_it.is_end());
      auto reg = field_regs.at(field);
      auto dest = move_result_pseudo_it->insn->dest();
      auto move_insn =
          (new IRInstruction(opcode::move_opcode(field->get_type())))
              ->set_src(0, reg)
              ->set_dest(dest);
      mutation.replace(it, {move_insn});
    }

    // Use the mutated copied ctor code to concretize the expanded ctor.
    mutation.flush();
    expanded_ctor_ref->make_concrete(ACC_CONSTRUCTOR | ACC_PUBLIC,
                                     std::move(cloned_code), false);
    auto expanded_ctor = expanded_ctor_ref->as_def();
    always_assert(expanded_ctor);
    expanded_ctor->rstate.set_generated();
    int api_level = api::LevelChecker::get_method_level(ctor);
    expanded_ctor->rstate.set_api_level(api_level);
    expanded_ctor->set_deobfuscated_name(show_deobfuscated(expanded_ctor));
    return expanded_ctor;
  }

 public:
  explicit ExpandableConstructorParams(const Scope& scope) {
    walk::classes(scope, [&](DexClass* cls) {
      for (auto ctor : cls->get_ctors()) {
        auto deob = ctor->get_deobfuscated_name_or_null();
        if (deob) {
          m_deobfuscated_ctor_names.insert(deob);
        }
      }
    });
  }

  // Try to create a method-ref that represents an expanded ctor, where a
  // particular parameter representing a (newly created) object gets replaced by
  // a sequence of field values used by the ctor.
  DexMethodRef* get_expanded_ctor_ref(DexMethod* ctor,
                                      param_index_t param_index,
                                      std::vector<DexField*>** fields) const {
    auto type = ctor->get_class();
    auto class_info = get_class_info(type);
    auto it = class_info->find(ctor);
    if (it == class_info->end()) {
      return nullptr;
    }
    auto it2 = it->second.find(param_index);
    if (it2 == it->second.end()) {
      return nullptr;
    }

    auto name = ctor->get_name();
    auto args_vector = get_expanded_args_vector(ctor, param_index, it2->second);
    auto type_list = DexTypeList::make_type_list(std::move(args_vector));
    auto proto = DexProto::make_proto(type::_void(), type_list);

    auto deob = show_deobfuscated(type, name, proto);
    if (m_deobfuscated_ctor_names.count(DexString::make_string(deob))) {
      // Some other method ref already has the synthetic deobfuscated name that
      // we'd later want to give to the new generated ctor.
      return nullptr;
    }

    std::lock_guard<std::mutex> lock_guard(m_candidates_mutex);
    auto expanded_ctor_ref = DexMethod::get_method(type, name, proto);
    if (expanded_ctor_ref) {
      if (!m_candidates.count(expanded_ctor_ref)) {
        // There's already a pre-existing method registered, maybe a method that
        // became unreachable. As other Redex optimizations might have persisted
        // this method-ref, we don't want to interact with it.
        return nullptr;
      }
    } else {
      expanded_ctor_ref = DexMethod::make_method(type, name, proto);
      always_assert(show_deobfuscated(expanded_ctor_ref) == deob);
      auto emplaced =
          m_candidates
              .emplace(expanded_ctor_ref, std::make_pair(ctor, param_index))
              .second;
      always_assert(emplaced);
    }
    *fields = &it2->second;
    return expanded_ctor_ref;
  }

  // Make sure that all newly used expanded ctors actually exist as concrete
  // methods.
  void flush(const Scope& scope, Stats* stats) {
    // First, find all expanded_ctor_ref that made it into the updated code.
    ConcurrentSet<DexMethodRef*> used_expanded_ctor_refs;
    walk::parallel::opcodes(scope, [&](DexMethod*, IRInstruction* insn) {
      if (opcode::is_invoke_direct(insn->opcode()) &&
          m_candidates.count(insn->get_method())) {
        used_expanded_ctor_refs.insert(insn->get_method());
      }
    });

    // Second, make them all concrete.
    ConcurrentSet<DexMethod*> expanded_ctors;
    workqueue_run<DexMethodRef*>(
        [&](DexMethodRef* expanded_ctor_ref) {
          expanded_ctors.insert(make_expanded_ctor_concrete(expanded_ctor_ref));
        },
        used_expanded_ctor_refs);

    // Add the newly concretized ctors to their classes.
    std::vector<DexMethod*> ordered(expanded_ctors.begin(),
                                    expanded_ctors.end());
    std::sort(ordered.begin(), ordered.end(), compare_dexmethods);
    for (auto expanded_ctor : ordered) {
      type_class(expanded_ctor->get_class())->add_method(expanded_ctor);
    }

    // Finally, erase the unused ctor method refs.
    for (auto [ctor, param_index] : m_candidates) {
      if (!used_expanded_ctor_refs.count(ctor)) {
        DexMethod::erase_method(ctor);
        DexMethod::delete_method_DO_NOT_USE(static_cast<DexMethod*>(ctor));
      }
    }

    stats->expanded_ctors += expanded_ctors.size();
  }
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

  int get_value() const {
    int net_savings{local};
    for (auto method : methods) {
      auto code_size = get_code_size(method);
      net_savings += 20 + code_size;
    }
    for (auto type : classes) {
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

// Data structure that represents a (cloned) reduced method, together with some
// auxiliary information that allows to derive net savings.
struct ReducedMethod {
  DexMethod* method;
  size_t initial_code_size;
  std::unordered_map<DexMethod*, std::unordered_set<DexType*>> inlined_methods;
  InlinableTypes types;

  NetSavings get_net_savings(
      const std::unordered_set<DexType*>& irreducible_types,
      NetSavings* conditional_net_savings = nullptr) const {
    auto final_code_size = get_code_size(method);
    NetSavings net_savings;
    net_savings.local = (int)initial_code_size - (int)final_code_size;

    std::unordered_set<DexType*> remaining;
    for (auto& [inlined_method, inlined_types] : inlined_methods) {
      if (!can_delete(inlined_method)) {
        remaining.insert(inlined_types.begin(), inlined_types.end());
        continue;
      }
      bool any_remaining = false;
      for (auto type : inlined_types) {
        if (types.at(type) || irreducible_types.count(type)) {
          remaining.insert(type);
          any_remaining = true;
        }
      }
      if (any_remaining) {
        if (conditional_net_savings) {
          conditional_net_savings->methods.insert(inlined_method);
        }
        continue;
      }
      net_savings.methods.insert(inlined_method);
    }

    for (auto [type, multiples] : types) {
      if (remaining.count(type) || multiples || irreducible_types.count(type)) {
        if (conditional_net_savings) {
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
  const ExpandableConstructorParams& m_expandable_constructor_params;
  MultiMethodInliner& m_inliner;
  const MethodSummaries& m_method_summaries;
  Stats* m_stats;
  bool m_is_init_or_clinit;
  DexMethod* m_method;
  const InlinableTypes& m_types;

 public:
  RootMethodReducer(
      const ExpandableConstructorParams& expandable_constructor_params,
      MultiMethodInliner& inliner,
      const MethodSummaries& method_summaries,
      Stats* stats,
      bool is_init_or_clinit,
      DexMethod* method,
      const InlinableTypes& types)
      : m_expandable_constructor_params(expandable_constructor_params),
        m_inliner(inliner),
        m_method_summaries(method_summaries),
        m_stats(stats),
        m_is_init_or_clinit(is_init_or_clinit),
        m_method(method),
        m_types(types) {}

  std::optional<ReducedMethod> reduce() {
    auto initial_code_size{get_code_size(m_method)};

    if (!inline_anchors() || !expand_or_inline_invokes()) {
      return std::nullopt;
    }

    while (auto* insn = find_inlinable_new_instance()) {
      if (!stackify(insn)) {
        return std::nullopt;
      }
    }

    shrink();
    return (ReducedMethod){m_method, initial_code_size,
                           std::move(m_inlined_methods), m_types};
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

  // Given a constructor invocation, replace a particular argument with the
  // sequence of the argument's field values to flow into an expanded
  // constructor.
  DexMethodRef* expand_invoke(cfg::CFGMutation& mutation,
                              const cfg::InstructionIterator& it,
                              param_index_t param_index) {
    auto insn = it->insn;
    auto callee = resolve_method(insn->get_method(), opcode_to_search(insn));
    always_assert(callee);
    always_assert(callee->is_concrete());
    std::vector<DexField*>* fields;
    auto expanded_ctor_ref =
        m_expandable_constructor_params.get_expanded_ctor_ref(
            callee, param_index, &fields);
    if (!expanded_ctor_ref) {
      return nullptr;
    }

    insn->set_method(expanded_ctor_ref);
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
    return expanded_ctor_ref;
  }

  bool expand_invokes(const std::unordered_map<IRInstruction*, param_index_t>&
                          invokes_to_expand,
                      std::unordered_set<DexMethodRef*>* expanded_ctor_refs) {
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
      auto expanded_ctor_ref = expand_invoke(mutation, it, param_index);
      if (!expanded_ctor_ref) {
        return false;
      }
      expanded_ctor_refs->insert(expanded_ctor_ref);
    }
    mutation.flush();
    return true;
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
        auto [callee, type] = resolve_inlinable(m_method_summaries, insn);
        if (!callee) {
          continue;
        }
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

  // Expand or inline all uses of all relevant new-instance instructions that
  // involve invoke- instructions, until there are no more such uses.
  bool expand_or_inline_invokes() {
    auto& cfg = m_method->get_code()->cfg();
    std::unordered_set<DexMethodRef*> expanded_ctor_refs;
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
        for (auto& use : uses) {
          auto emplaced =
              aggregated_uses[use.insn].emplace(use.src_index, type).second;
          always_assert(emplaced);
        }
      }
      for (auto& [uses_insn, src_indices] : aggregated_uses) {
        if (!opcode::is_an_invoke(uses_insn->opcode()) ||
            is_benign(uses_insn->get_method())) {
          continue;
        }
        if (expanded_ctor_refs.count(uses_insn->get_method())) {
          m_stats->invokes_not_inlinable_callee_inconcrete++;
          return false;
        }
        if (method::is_init(uses_insn->get_method()) && !src_indices.empty() &&
            !src_indices.count(0)) {
          if (src_indices.size() > 1) {
            m_stats
                ->invokes_not_inlinable_callee_is_init_too_many_params_to_expand++;
            return false;
          }
          always_assert(src_indices.size() == 1);
          auto src_index = src_indices.begin()->first;
          invokes_to_expand.emplace(uses_insn, src_index);
          continue;
        }

        auto callee = resolve_method(uses_insn->get_method(),
                                     opcode_to_search(uses_insn));
        always_assert(callee);
        always_assert(callee->is_concrete());
        invokes_to_inline.insert(uses_insn);
        for (auto [src_index, type] : src_indices) {
          m_inlined_methods[callee].insert(type);
        }
      }

      if (!expand_invokes(invokes_to_expand, &expanded_ctor_refs)) {
        m_stats->invokes_not_inlinable_callee_is_init_unexpandable++;
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

// Reduce all root methods to a set of variants. The reduced methods are ordered
// by how many types where inlined, with the largest number of inlined types
// going first.
std::unordered_map<DexMethod*, std::vector<ReducedMethod>>
compute_reduced_methods(
    ExpandableConstructorParams& expandable_constructor_params,
    MultiMethodInliner& inliner,
    const MethodSummaries& method_summaries,
    const std::unordered_map<DexMethod*, InlinableTypes>& root_methods,
    std::unordered_set<DexType*>* irreducible_types,
    Stats* stats) {
  // We are not exploring all possible subsets of types, but only single chain
  // of subsets, guided by whether types have multiple uses, and by how often
  // they appear as inlinable types in root methods.
  // TODO: Explore all possible subsets of types.
  std::unordered_map<DexType*, size_t> occurrences;
  for (auto&& [method, types] : root_methods) {
    for (auto [type, multiples] : types) {
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
    // We sort types with multiple uses to the front.
    if (p.second != q.second) {
      return p.second > q.second;
    }
    // We sort types with more frequent occrrences to the front.
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
    std::vector<std::pair<DexType*, bool>> ordered_types(types.begin(),
                                                         types.end());
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
        RootMethodReducer root_method_reducer{expandable_constructor_params,
                                              inliner,
                                              method_summaries,
                                              stats,
                                              method::is_init(method) ||
                                                  method::is_clinit(method),
                                              copy,
                                              types};
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
    for (auto&& [type, multiples] : types) {
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
        // We'll try to find the maximal (involving most inlined types) reduced
        // method variant for which we can make a a non-negative local cost
        // determination.
        for (size_t i = 0; i < reduced_methods_variants.size(); i++) {
          // If we couldn't accomodate any types, we'll need to add them to the
          // irreducible types set. Except for the last variant with the least
          // inlined types, for which might below try to make a global cost
          // determination.
          const auto& reduced_method = reduced_methods_variants.at(i);
          if (i > 0) {
            for (auto&& [type, multiples] :
                 reduced_methods_variants.at(i - 1).types) {
              if (!reduced_method.types.count(type)) {
                concurrent_irreducible_types.insert(type);
              }
            }
          }
          auto local_net_savings =
              reduced_method.get_net_savings(*irreducible_types);
          if (local_net_savings.get_value() >= 0) {
            stats->total_savings += local_net_savings.get_value();
            concurrent_selected_reduced_methods.emplace(method, i);
            return;
          }
        }
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
        for (auto [type, multiples] : smallest_reduced_method.types) {
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
            const MethodSummaries& method_summaries,
            const std::unordered_map<DexMethod*, InlinableTypes>& root_methods,
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

  // First, we compute all reduced methods

  ExpandableConstructorParams expandable_constructor_params(scope);
  std::unordered_set<DexType*> irreducible_types;
  auto reduced_methods = compute_reduced_methods(
      expandable_constructor_params, inliner, method_summaries, root_methods,
      &irreducible_types, stats);
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
        }
        stats->reduced_methods_variants += reduced_methods_variants.size();
        for (auto& reduced_method : reduced_methods_variants) {
          DexMethod::erase_method(reduced_method.method);
          DexMethod::delete_method_DO_NOT_USE(reduced_method.method);
        }
      },
      reduced_methods);

  expandable_constructor_params.flush(scope, stats);
}
} // namespace

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
  reduce(stores, scope, conf, method_summaries, root_methods, &stats);

  walk::parallel::code(scope,
                       [&](DexMethod*, IRCode& code) { code.clear_cfg(); });

  TRACE(OEA, 1, "[object escape analysis] total savings: %zu",
        (size_t)stats.total_savings);
  TRACE(OEA, 1,
        "[object escape analysis] %zu root methods lead to %zu reduced root "
        "methods with %zu variants "
        "of which %zu were selected and %zu anchors not inlinable because "
        "inlining failed, %zu/%zu invokes not inlinable because callee is "
        "init, %zu invokes not inlinable because callee is not concrete,"
        "%zu invokes not inlinable because inlining failed, %zu invokes not "
        "inlinable after too many iterations, %zu stackify returned objects, "
        "%zu too costly with irreducible classes, %zu too costly with multiple "
        "conditional classes, %zu too costly globally; %zu expanded ctors",
        root_methods.size(), (size_t)stats.reduced_methods,
        (size_t)stats.reduced_methods_variants,
        (size_t)stats.selected_reduced_methods,
        (size_t)stats.anchors_not_inlinable_inlining,
        (size_t)stats.invokes_not_inlinable_callee_is_init_unexpandable,
        (size_t)stats
            .invokes_not_inlinable_callee_is_init_too_many_params_to_expand,
        (size_t)stats.invokes_not_inlinable_callee_inconcrete,
        (size_t)stats.invokes_not_inlinable_inlining,
        (size_t)stats.invokes_not_inlinable_too_many_iterations,
        (size_t)stats.stackify_returns_objects,
        (size_t)stats.too_costly_irreducible_classes,
        (size_t)stats.too_costly_multiple_conditional_classes,
        (size_t)stats.too_costly_globally, (size_t)stats.expanded_ctors);

  mgr.incr_metric("total_savings", stats.total_savings);
  mgr.incr_metric("root_methods", root_methods.size());
  mgr.incr_metric("reduced_methods", (size_t)stats.reduced_methods);
  mgr.incr_metric("reduced_methods_variants",
                  (size_t)stats.reduced_methods_variants);
  mgr.incr_metric("selected_reduced_methods",
                  (size_t)stats.selected_reduced_methods);
  mgr.incr_metric("root_method_anchors_not_inlinable_inlining",
                  (size_t)stats.anchors_not_inlinable_inlining);
  mgr.incr_metric(
      "root_method_invokes_not_inlinable_callee_is_init_unexpandable",
      (size_t)stats.invokes_not_inlinable_callee_is_init_unexpandable);
  mgr.incr_metric(
      "root_method_invokes_not_inlinable_callee_is_init_too_many_params_to_"
      "expand",
      (size_t)
          stats.invokes_not_inlinable_callee_is_init_too_many_params_to_expand);
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
  mgr.incr_metric("expanded_ctors", (size_t)stats.expanded_ctors);
}

static ObjectEscapeAnalysisPass s_pass;
