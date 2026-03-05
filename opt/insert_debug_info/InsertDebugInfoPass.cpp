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

namespace {

void insert_debug_info(DexMethod* method,
                       IRCode& code,
                       cfg::ControlFlowGraph& cfg) {
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
}

namespace entry_cover {

enum class CoverResult {
  kNotNeeded,
  kDone,
  kFileAmbiguity,
  kNoPosition,
};

using FindResultType =
    std::variant<const DexPosition*, std::monostate, CoverResult>;

FindResultType find_smallest_pos_filtered(cfg::ControlFlowGraph& cfg,
                                          const auto& filter_fn) {
  std::optional<const DexPosition*> smallest_pos{};
  for (const auto* b : cfg.blocks_view()) {
    for (const auto& mie : *b) {
      if (mie.type == MFLOW_POSITION) {
        if (!filter_fn(mie.pos.get())) {
          continue;
        }

        if (!smallest_pos) {
          smallest_pos = mie.pos.get();
          continue;
        }

        const auto* pos = mie.pos.get();
        if (pos->file != (*smallest_pos)->file) {
          return CoverResult::kFileAmbiguity;
        }
        if (pos->line < (*smallest_pos)->line) {
          smallest_pos = pos;
        }
      }
    }
  }
  if (smallest_pos) {
    return *smallest_pos;
  }
  return std::monostate{};
}

CoverResult maybe_insert(DexMethod* method, cfg::ControlFlowGraph& cfg) {
  auto* entry_block = cfg.entry_block();
  auto needs_injection = [entry_block]() {
    for (const auto& mie : *entry_block) {
      if (mie.type == MFLOW_POSITION) {
        return false;
      }
      if (mie.type == MFLOW_OPCODE &&
          !opcode::is_a_load_param(mie.insn->opcode())) {
        return true;
      }
    }
    return true;
  }();
  if (!needs_injection) {
    return CoverResult::kNotNeeded;
  }

  auto smallest_pos = [&cfg, method]() -> FindResultType {
    // Restrict to matching method.
    {
      auto name = show(method);
      std::string_view name_view(name);
      auto smallest_matching_method =
          find_smallest_pos_filtered(cfg, [&name_view](const DexPosition* pos) {
            return pos->method != nullptr && pos->method->str() == name_view;
          });
      if (std::holds_alternative<const DexPosition*>(
              smallest_matching_method)) {
        const auto* pos =
            std::get<const DexPosition*>(smallest_matching_method);
        redex_assert(pos != nullptr);
        return pos;
      }
    }

    // Generically find
    return find_smallest_pos_filtered(cfg,
                                      [](const DexPosition*) { return true; });
  }();

  if (std::holds_alternative<CoverResult>(smallest_pos)) {
    return std::get<CoverResult>(smallest_pos);
  }
  if (std::holds_alternative<std::monostate>(smallest_pos)) {
    return CoverResult::kNoPosition;
  }

  auto it = entry_block->get_first_non_param_loading_insn();
  const auto* pos = std::get<const DexPosition*>(smallest_pos);
  cfg.insert_before(
      entry_block, it,
      std::make_unique<DexPosition>(pos->method, pos->file, pos->line));
  return CoverResult::kDone;
}

} // namespace entry_cover
} // namespace

void InsertDebugInfoPass::run_pass(DexStoresVector& stores,
                                   ConfigFiles& /*conf*/,
                                   PassManager& mgr) {
  std::atomic<uint32_t> patched_method = 0;
  std::atomic<uint32_t> entry_injected = 0;
  std::atomic<uint32_t> entry_injection_failed = 0;
  std::atomic<uint32_t> entry_injection_no_position = 0;
  walk::parallel::code(build_class_scope(stores),
                       [&](DexMethod* method, IRCode& code) {
                         always_assert(code.cfg_built());
                         auto& cfg = code.cfg();

                         // First check whether entry cover is needed. If so,
                         // or if it fails, it still means the method has
                         // debug info, so can skip the injection.
                         {
                           using namespace entry_cover;
                           auto res = entry_cover::maybe_insert(method, cfg);
                           switch (res) {
                           case CoverResult::kNotNeeded:
                             return;
                           case CoverResult::kDone:
                             entry_injected++;
                             return;
                           case CoverResult::kFileAmbiguity:
                             entry_injection_failed++;
                             return;
                           case CoverResult::kNoPosition:
                             entry_injection_no_position++;
                             // Fall through to generic insertion.
                             break;
                           }
                         }

                         patched_method++;
                         insert_debug_info(method, code, cfg);
                       });

  mgr.set_metric("patched_method", patched_method);
  mgr.set_metric("entry_injected", entry_injected);
  mgr.set_metric("entry_injection_failed", entry_injection_failed);
  mgr.set_metric("entry_injection_no_position", entry_injection_no_position);
}

static InsertDebugInfoPass s_pass;
