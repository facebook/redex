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
#include "PassManager.h"
#include "Resolver.h"
#include "TypeInference.h"
#include "TypeSystem.h"
#include "Util.h"
#include "Walkers.h"

namespace {

boost::optional<DexMethod*> get_method_def_from(const VirtualScope* v_scope,
                                                const DexType* inferred_type) {
  for (const auto& v_pair : v_scope->methods) {
    auto m = v_pair.first;
    if (m->get_class() == inferred_type && m->is_def()) {
      return boost::optional<DexMethod*>(m);
    }
  }

  return boost::none;
}

struct Stats {
  uint32_t num_invoke_virtual_replaced{0};
  uint32_t num_invoke_interface_replaced{0};
};

Stats replaced_virtual_refs(const TypeSystem& type_system, DexMethod* method) {
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

        boost::optional<DexMethod*> m_def = boost::none;
        if (is_invoke_virtual(insn->opcode())) {
          auto virtual_scope = type_system.find_virtual_scope(callee);
          m_def = get_method_def_from(virtual_scope, *dex_type);
        } else {
          auto intf_scope = type_system.find_interface_scope(callee);
          for (const auto& v_scope : intf_scope) {
            m_def = get_method_def_from(v_scope, *dex_type);
            if (m_def) {
              break;
            }
          }
        }

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
  TypeSystem type_system(scope);

  Stats stats = walk::parallel::reduce_methods<Stats>(
      scope,
      [&](DexMethod* method) -> Stats {
        return replaced_virtual_refs(type_system, method);
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
