/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InsertDebugInfoPass.h"

#include "ControlFlow.h"
#include "PassManager.h"
#include "Show.h"
#include "Walkers.h"

void InsertDebugInfoPass::run_pass(DexStoresVector& stores,
                                   ConfigFiles& conf,
                                   PassManager& mgr) {
  std::atomic<uint32_t> patched_method = 0;
  walk::parallel::code(
      build_class_scope(stores), [&](DexMethod* method, IRCode& code) {
        always_assert(code.editable_cfg_built());
        bool has_position = false;
        auto& cfg = code.cfg();
        for (auto* block : cfg.blocks()) {
          for (auto it = block->begin(); it != block->end(); it++) {
            if (it->type == MFLOW_POSITION) {
              has_position = true;
              break;
            }
          }
        }
        if (has_position) {
          return;
        }
        patched_method++;
        always_assert_log(!code.get_debug_item(),
                          "%s has no DexPosition, but has a DexDebugItem %s",
                          SHOW(method),
                          SHOW(code.cfg()));
        code.set_debug_item(std::make_unique<DexDebugItem>());
        auto* block = cfg.entry_block();
        auto last_param = block->get_last_param_loading_insn();
        auto artificial_pos = std::make_unique<DexPosition>(
            DexString::make_string(show_deobfuscated(method)),
            DexString::make_string("UnknownSource"), 0);
        if (last_param == block->end()) {
          cfg.insert_before(block->to_cfg_instruction_iterator(
                                block->get_first_non_param_loading_insn()),
                            std::move(artificial_pos));
        } else {
          cfg.insert_after(block->to_cfg_instruction_iterator(
                               block->get_last_param_loading_insn()),
                           std::move(artificial_pos));
        }
      });

  mgr.set_metric("patched_method", patched_method);
}

static InsertDebugInfoPass s_pass;
