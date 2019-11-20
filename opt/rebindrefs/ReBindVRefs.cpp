/*
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
  uint32_t num_desupered{0};

  Stats& operator+=(const Stats& that) {
    num_invoke_virtual_replaced += that.num_invoke_virtual_replaced;
    num_invoke_interface_replaced += that.num_invoke_interface_replaced;
    num_desupered += that.num_desupered;
    return *this;
  }
};

void try_desuperify(const DexMethod* caller,
                    IRInstruction* insn,
                    Stats* stats) {
  if (!is_invoke_super(insn->opcode())) {
    return;
  }
  auto cls = type_class(caller->get_class());
  if (cls == nullptr) {
    return;
  }
  // resolve_method_ref will start its search in the superclass of :cls.
  auto callee = resolve_method_ref(cls, insn->get_method()->get_name(),
                                   insn->get_method()->get_proto(),
                                   MethodSearch::Virtual);
  // External methods may not always be final across runtime versions
  if (callee == nullptr || callee->is_external() || !is_final(callee)) {
    return;
  }
  // Skip if the callee is an interface default method (037).
  auto callee_cls = type_class(callee->get_class());
  if (is_interface(callee_cls)) {
    return;
  }

  TRACE(BIND, 5, "Desuperifying %s because %s is final", SHOW(insn),
        SHOW(callee));
  insn->set_opcode(OPCODE_INVOKE_VIRTUAL);
  stats->num_desupered++;
}

Stats replaced_virtual_refs(const mog::Graph& override_graph,
                            DexMethod* method,
                            bool desuperify) {
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
    if (desuperify) {
      try_desuperify(method, insn, &stats);
    }

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
  Stats stats = walk::parallel::methods<Stats>(scope, [&](DexMethod* method) {
    return replaced_virtual_refs(*override_graph, method, m_desuperify);
  });
  mgr.set_metric("num_desupered", stats.num_desupered);
  mgr.set_metric("num_invoke_virtual_replaced",
                 stats.num_invoke_virtual_replaced);
  mgr.set_metric("num_invoke_interface_replaced",
                 stats.num_invoke_interface_replaced);
}

static ReBindVRefsPass s_pass;
