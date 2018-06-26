/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveUnusedArgs.h"

#include <unordered_map>
#include <vector>

#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "Walkers.h"

// NOTE:
//  TODO <anwangster> (*) indicates goal for next diff

namespace {

constexpr const char* METRIC_ARGS_REMOVED = "callsite_args_removed";

class RemoveArgs {

 public:
  RemoveArgs(const Scope& scope) : m_scope(scope){};

  int get_num_args_removed() { return m_num_args_removed; }

  void run() {
    update_meths_with_unused_args();
    update_callsites();
  }

 private:
  const Scope& m_scope;
  std::unordered_map<DexMethod*, std::vector<uint16_t>> m_dead_args_map;
  size_t m_num_args_removed;

  /**
   * Finds methods that have unusued arguments, and
   * records unused argument information in m_dead_args_map.
   * TODO <anwangster> think of shorter name
   */
  void update_meths_with_unused_args() {
    walk::methods(m_scope, [&](DexMethod* method) {
      auto code = method->get_code();
      if (code == nullptr) {
        return;
      }
      code->build_cfg();

      // TODO <anwangster> (*)
      //  perform liveness analysis on method's params
      //  update m_dead_args_map as necessary
      //  update method signature
      //  treat possible method signature collisions
    });
  }

  /**
   * Removes dead arguments from the given invoke instr.
   */
  void update_callsite(IRInstruction* instr) {
    // TODO <anwangster> (*)
    //  get the invoked method m and see if args are removable
    //  remove dead params from invocation args list
  }

  /**
   * Removes unused arguments at callsites.
   * Returns the number of arguments removed.
   */
  void update_callsites() {
    // walk through all methods to look for and edit callsites
    walk::methods(m_scope, [&](DexMethod* method) {
      auto code = method->get_code();
      if (code == nullptr) {
        return;
      }

      for (const auto& mie : InstructionIterable(code)) {
        auto insn = mie.insn;
        auto opcode = insn->opcode();

        if (opcode == OPCODE_INVOKE_DIRECT || opcode == OPCODE_INVOKE_STATIC) {
          update_callsite(insn);
          // TODO <anwangster> (*)
          //  determine any additional updates needed
        } else if (opcode == OPCODE_INVOKE_VIRTUAL) {
          // TODO <anwangster>
          //   maybe coalesce with above branch
        }
      }
    });
  }
};

} // namespace

namespace remove_unused_args {

void RemoveUnusedArgsPass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& cfg,
                                    PassManager& mgr) {
  auto scope = build_class_scope(stores);
  RemoveArgs rm_args(scope);
  rm_args.run();
  size_t num_args_removed = rm_args.get_num_args_removed();

  TRACE(ARGS,
        1,
        "ARGS :| Removed %d redundant callsite parameters\n",
        num_args_removed);

  mgr.set_metric(METRIC_ARGS_REMOVED, num_args_removed);
}

static RemoveUnusedArgsPass s_pass;

} // namespace remove_unused_args
