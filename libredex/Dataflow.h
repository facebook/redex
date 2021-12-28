/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ControlFlow.h"
#include "IRInstruction.h"

template <typename T>
std::unique_ptr<std::unordered_map<IRInstruction*, T>> forwards_dataflow(
    const std::vector<cfg::Block*>& blocks,
    const T& bottom,
    const std::function<void(IRList::iterator, T*)>& trans,
    const T& entry_value) {
  std::vector<T> block_outs(blocks.size(), bottom);
  std::deque<cfg::Block*> work_list(blocks.begin(), blocks.end());
  while (!work_list.empty()) {
    auto block = work_list.front();
    work_list.pop_front();
    auto insn_in = bottom;
    if (block->id() == 0) {
      insn_in = entry_value;
    }
    for (const auto& pred : block->preds()) {
      insn_in.meet(block_outs[pred->src()->id()]);
    }
    for (auto it = block->begin(); it != block->end(); ++it) {
      if (it->type != MFLOW_OPCODE) {
        continue;
      }
      trans(it, &insn_in);
    }
    if (insn_in != block_outs[block->id()]) {
      block_outs[block->id()] = std::move(insn_in);
      for (auto& succ : block->succs()) {
        if (std::find(work_list.begin(), work_list.end(), succ->target()) ==
            work_list.end()) {
          work_list.push_back(succ->target());
        }
      }
    }
  }

  auto insn_in_map = std::make_unique<std::unordered_map<IRInstruction*, T>>();
  for (const auto& block : blocks) {
    auto insn_in = bottom;
    if (block->id() == 0) {
      insn_in = entry_value;
    }
    for (const auto& pred : block->preds()) {
      insn_in.meet(block_outs[pred->src()->id()]);
    }
    for (auto it = block->begin(); it != block->end(); ++it) {
      if (it->type != MFLOW_OPCODE) {
        continue;
      }
      IRInstruction* insn = it->insn;
      insn_in_map->emplace(insn, insn_in);
      trans(it, &insn_in);
    }
  }

  return insn_in_map;
}

template <typename T>
std::unique_ptr<std::unordered_map<IRInstruction*, T>> forwards_dataflow(
    const std::vector<cfg::Block*>& blocks,
    const T& bottom,
    const std::function<void(IRList::iterator, T*)>& trans) {
  return forwards_dataflow(blocks, bottom, trans, bottom);
}
