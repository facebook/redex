/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReBindVRefs.h"

#include <boost/optional.hpp>

#include "DexClass.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "MethodOverrideGraph.h"
#include "PassManager.h"
#include "Resolver.h"
#include "TypeInference.h"
#include "Util.h"
#include "Walkers.h"

namespace mog = method_override_graph;

namespace {

boost::optional<DexMethod*> get_method_def_from(
    const mog::Graph& override_graph,
    DexMethod* callee,
    const DexType* inferred_type) {
  auto overrides = mog::get_overriding_methods(override_graph, callee);
  for (auto* m : overrides) {
    if (m->get_class() == inferred_type && m->is_def()) {
      return boost::optional<DexMethod*>(const_cast<DexMethod*>(m));
    }
  }
  return boost::none;
}

struct Stats {
  uint32_t num_invoke_virtual_replaced{0};
  uint32_t num_invoke_interface_replaced{0};
};

Stats replaced_virtual_refs(const mog::Graph& override_graph,
                            DexMethod* method) {
  Stats stats;
  if (!method || !method->get_code()) {
    return stats;
  }

  auto* code = method->get_code();
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  type_inference::TypeInference inference(cfg);
  inference.run(method);
  auto& envs = inference.get_type_environments();

  for (auto& mie : InstructionIterable(code)) {
    IRInstruction* insn = mie.insn;
    auto opcode = insn->opcode();
    if (is_invoke_virtual(opcode) || opcode == OPCODE_INVOKE_INTERFACE) {

      auto callee = resolve_method(insn->get_method(), opcode_to_search(insn));
      if (!callee) {
        continue;
      }

      auto this_reg = insn->src(0);
      auto& env = envs.at(insn);
      auto dex_type = env.get_dex_type(this_reg);

      if (dex_type && callee->get_class() != *dex_type) {
        // replace it with the actual implementation if any provided.
        auto m_def = get_method_def_from(override_graph, callee, *dex_type);
        if (m_def) {
          insn->set_method(*m_def);
          if (opcode == OPCODE_INVOKE_INTERFACE) {
            insn->set_opcode(OPCODE_INVOKE_VIRTUAL);
            stats.num_invoke_interface_replaced++;
          } else {
            stats.num_invoke_virtual_replaced++;
          }
        }
      }
    }
  }

  return stats;
}

} // namespace

void ReBindVRefsPass::run_pass(DexStoresVector& stores,
                               ConfigFiles& /* conf */,
                               PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  // We want to be able to rebind external refs as well, so make sure they are
  // included in the MOG.
  std::vector<DexClass*> full_scope{scope};
  full_scope.insert(full_scope.end(),
                    g_redex->external_classes().begin(),
                    g_redex->external_classes().end());
  auto override_graph = mog::build_graph(full_scope);

  Stats stats = walk::parallel::reduce_methods<Stats>(
      scope,
      [&](DexMethod* method) -> Stats {
        return replaced_virtual_refs(*override_graph, method);
      },
      [](Stats a, Stats b) {
        a.num_invoke_virtual_replaced += b.num_invoke_virtual_replaced;
        a.num_invoke_interface_replaced += b.num_invoke_interface_replaced;
        return a;
      });
  mgr.set_metric("num_invoke_virtual_replaced",
                 stats.num_invoke_virtual_replaced);
  mgr.set_metric("num_invoke_interface_replaced",
                 stats.num_invoke_interface_replaced);
}

static ReBindVRefsPass s_pass;
