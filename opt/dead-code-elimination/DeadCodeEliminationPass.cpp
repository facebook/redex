/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DeadCodeEliminationPass.h"

#include "DexUtil.h"
#include "LocalPointersAnalysis.h"
#include "SummarySerialization.h"
#include "Transform.h"
#include "VirtualScope.h"
#include "Walkers.h"

/*
 * This pass tries to identify writes to registers and objects that never get
 * read from. Modeling dead object field writes is particularly useful in
 * conjunction with RemoveUnreadFieldsPass. Suppose we have an unused field
 * Foo.x:
 *
 *   new-instance v0 LFoo;
 *   invoke-direct {v0} LFoo;.<init>()V
 *   sput-object v0 LBar;.x:LFoo; # RMUF will remove this
 *
 * If we can determine that Foo's constructor does not modify anything
 * outside of its `this` argument, we will be able to remove the invoke-direct
 * call as well as the new-instance instruction.
 *
 * In contrast, LocalDce can only identify unused writes to registers -- it
 * knows nothing about objects. The trade-off is that this is takes much longer
 * to run.
 */

namespace ptrs = local_pointers;

class CallGraphStrategy final : public call_graph::BuildStrategy {
 public:
  CallGraphStrategy(const Scope& scope)
      : m_scope(scope),
        m_non_overridden_virtuals(find_non_overridden_virtuals(scope)) {}

  call_graph::CallSites get_callsites(const DexMethod* method) const override {
    call_graph::CallSites callsites;
    auto* code = const_cast<IRCode*>(method->get_code());
    if (code == nullptr) {
      return callsites;
    }
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (is_invoke(insn->opcode())) {
        auto callee = resolve_method(
            insn->get_method(), opcode_to_search(insn), m_resolved_refs);
        if (callee == nullptr || may_be_overridden(callee)) {
          continue;
        }
        callsites.emplace_back(callee, code->iterator_to(mie));
      }
    }
    return callsites;
  }

  // XXX(jezng): We make every single method a root in order that all methods
  // are seen as reachable. Unreachable methods will not have `get_callsites`
  // run on them and will not have their outgoing edges added to the call graph,
  // which means that the dead code removal will not optimize them fully. I'm
  // not sure why these "unreachable" methods are not ultimately removed by RMU,
  // but as it stands, properly optimizing them is a size win for us.
  std::vector<DexMethod*> get_roots() const override {
    std::vector<DexMethod*> roots;

    walk::code(m_scope, [&](DexMethod* method, IRCode& code) {
      roots.emplace_back(method);
    });
    return roots;
  }

 private:
  bool may_be_overridden(DexMethod* method) const {
    return method->is_virtual() && m_non_overridden_virtuals.count(method) == 0;
  }

  const Scope& m_scope;
  std::unordered_set<const DexMethod*> m_non_overridden_virtuals;
  mutable MethodRefCache m_resolved_refs;
};

void DeadCodeEliminationPass::run_pass(DexStoresVector& stores,
                                       ConfigFiles&,
                                       PassManager&) {
  auto scope = build_class_scope(stores);

  walk::parallel::code(scope, [&](const DexMethod* method, IRCode& code) {
    code.build_cfg(/* editable */ false);
    // The backwards uv::FixpointIterator analysis will need it later.
    code.cfg().calculate_exit_block();
  });

  auto call_graph = call_graph::Graph(CallGraphStrategy(scope));
  auto ptrs_fp_iter_map = ptrs::analyze_scope(scope, call_graph);

  side_effects::SummaryMap effect_summaries;
  if (m_external_summaries_file) {
    std::ifstream file_input(*m_external_summaries_file);
    summary_serialization::read(file_input, &effect_summaries);
  }
  side_effects::analyze_scope(
      scope, call_graph, *ptrs_fp_iter_map, &effect_summaries);

  auto non_overridden_virtuals = find_non_overridden_virtuals(scope);
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    auto ptrs_fp_iter = ptrs_fp_iter_map->find(method)->second;
    UsedVarsFixpointIterator used_vars_fp_iter(*ptrs_fp_iter,
                                               effect_summaries,
                                               non_overridden_virtuals,
                                               code.cfg());
    used_vars_fp_iter.run(UsedVarsSet());

    TRACE(DEAD_CODE, 5, "Transforming %s\n", SHOW(method));
    TRACE(DEAD_CODE, 5, "Before:\n%s\n", SHOW(code.cfg()));
    auto dead_instructions = get_dead_instructions(&code, used_vars_fp_iter);
    for (auto dead : dead_instructions) {
      code.remove_opcode(dead);
    }
    transform::remove_unreachable_blocks(&code);
    TRACE(DEAD_CODE, 5, "After:\n%s\n", SHOW(&code));
  });
}

static DeadCodeEliminationPass s_pass;
