/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ObjectEscapeAnalysisImpl.h"

#include "Walkers.h"

using namespace sparta;
namespace mog = method_override_graph;

namespace {
constexpr const IRInstruction* NO_ALLOCATION = nullptr;
} // namespace

namespace object_escape_analysis_impl {

std::unordered_set<DexClass*> get_excluded_classes(
    const mog::Graph& method_override_graph) {
  std::unordered_set<DexClass*> res;
  for (auto* overriding_method : method_override_graph::get_overriding_methods(
           method_override_graph, method::java_lang_Object_finalize())) {
    auto* cls = type_class(overriding_method->get_class());
    if (cls && !cls->is_external()) {
      res.insert(cls);
    }
  }
  return res;
};

// Collect all allocation and invoke instructions, as well as non-virtual
// invocation dependencies.
void analyze_scope(
    const Scope& scope,
    const mog::Graph& method_override_graph,
    ConcurrentMap<DexType*, Locations>* new_instances,
    ConcurrentMap<DexMethod*, Locations>* invokes,
    ConcurrentMap<DexMethod*, std::unordered_set<DexMethod*>>* dependencies) {
  Timer t("analyze_scope");
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    always_assert(code.editable_cfg_built());
    LazyUnorderedMap<DexMethod*, bool> is_not_overridden([&](auto* m) {
      return !m->is_virtual() ||
             !mog::any_overriding_methods(method_override_graph, m);
    });
    for (auto& mie : InstructionIterable(code.cfg())) {
      auto insn = mie.insn;
      if (insn->opcode() == OPCODE_NEW_INSTANCE) {
        auto cls = type_class(insn->get_type());
        if (cls && !cls->is_external()) {
          new_instances->update(insn->get_type(), [&](auto*, auto& vec, bool) {
            vec.emplace_back(method, insn);
          });
        }
      } else if (opcode::is_an_invoke(insn->opcode())) {
        auto callee =
            resolve_method(insn->get_method(), opcode_to_search(insn));
        if (callee && callee->get_code() && !callee->is_external() &&
            is_not_overridden[callee]) {
          invokes->update(callee, [&](auto*, auto& vec, bool) {
            vec.emplace_back(method, insn);
          });
          if (is_not_overridden[method]) {
            dependencies->update(callee, [method](auto, auto& set, auto) {
              set.insert(method);
            });
          }
        }
      }
    }
  });
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

Analyzer::Analyzer(const std::unordered_set<DexClass*>& excluded_classes,
                   const MethodSummaries& method_summaries,
                   DexMethodRef* incomplete_marker_method,
                   cfg::ControlFlowGraph& cfg)
    : BaseIRAnalyzer(cfg),
      m_excluded_classes(excluded_classes),
      m_method_summaries(method_summaries),
      m_incomplete_marker_method(incomplete_marker_method) {
  MonotonicFixpointIterator::run(Environment::top());
}

const IRInstruction* Analyzer::get_singleton_allocation(const Domain& domain) {
  always_assert(domain.kind() == AbstractValueKind::Value);
  auto& elements = domain.elements();
  if (elements.size() != 1) {
    return nullptr;
  }
  return *elements.begin();
}

void Analyzer::analyze_instruction(const IRInstruction* insn,
                                   Environment* current_state) const {

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
    if (cls && !cls->is_external() && !m_excluded_classes.count(cls)) {
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

// Returns set of new-instance and invoke- allocating instructions that do not
// escape (or return).
std::unordered_set<IRInstruction*> Analyzer::get_inlinables() {
  std::unordered_set<IRInstruction*> inlinables;
  for (auto&& [insn, uses] : m_escapes) {
    if (uses.empty() && insn->opcode() != IOPCODE_LOAD_PARAM_OBJECT &&
        !m_returns.count(insn)) {
      inlinables.insert(const_cast<IRInstruction*>(insn));
    }
  }
  return inlinables;
}

MethodSummaries compute_method_summaries(
    const Scope& scope,
    const ConcurrentMap<DexMethod*, std::unordered_set<DexMethod*>>&
        dependencies,
    const mog::Graph& method_override_graph,
    const std::unordered_set<DexClass*>& excluded_classes,
    size_t* analysis_iterations) {
  Timer t("compute_method_summaries");

  std::unordered_set<DexMethod*> impacted_methods;
  walk::code(scope, [&](DexMethod* method, IRCode&) {
    if (!method->is_virtual() ||
        !mog::any_overriding_methods(method_override_graph, method)) {
      impacted_methods.insert(method);
    }
  });

  MethodSummaries method_summaries;
  *analysis_iterations = 0;
  while (!impacted_methods.empty()) {
    Timer t2("analysis iteration");
    (*analysis_iterations)++;
    TRACE(OEA, 2, "[object escape analysis] analysis_iteration %zu",
          *analysis_iterations);
    ConcurrentMap<DexMethod*, MethodSummary> recomputed_method_summaries;
    workqueue_run<DexMethod*>(
        [&](DexMethod* method) {
          auto& cfg = method->get_code()->cfg();
          Analyzer analyzer(excluded_classes, method_summaries,
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
          if (returns.size() == 1) {
            const auto* allocation_insn = *returns.begin();
            if (allocation_insn != NO_ALLOCATION &&
                escapes.at(allocation_insn).empty() &&
                allocation_insn->opcode() != IOPCODE_LOAD_PARAM_OBJECT) {
              recomputed_method_summaries.update(
                  method, [allocation_insn](DexMethod*, auto& ms, bool) {
                    ms.allocation_insn = allocation_insn;
                  });
            }
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

} // namespace object_escape_analysis_impl
