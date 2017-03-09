/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ControlFlow.h"
#include "DexInstruction.h"

template <typename T>
std::unique_ptr<std::unordered_map<DexInstruction*, T>> forwards_dataflow(
    const std::vector<Block*>& blocks,
    const T& bottom,
    const std::function<void(const DexInstruction*, T*)>& trans) {
  std::vector<T> block_outs(blocks.size(), bottom);
  std::deque<Block*> work_list(blocks.begin(), blocks.end());
  while (!work_list.empty()) {
    auto block = work_list.front();
    work_list.pop_front();
    auto insn_in = bottom;
    for (Block* pred : block->preds()) {
      insn_in.meet(block_outs[pred->id()]);
    }
    for (auto it = block->begin(); it != block->end(); ++it) {
      if (it->type != MFLOW_OPCODE) {
        continue;
      }
      DexInstruction* insn = it->insn;
      trans(insn, &insn_in);
    }
    if (insn_in != block_outs[block->id()]) {
      block_outs[block->id()] = std::move(insn_in);
      for (auto succ : block->succs()) {
        if (std::find(work_list.begin(), work_list.end(), succ) ==
            work_list.end()) {
          work_list.push_back(succ);
        }
      }
    }
  }

  auto insn_in_map =
      std::make_unique<std::unordered_map<DexInstruction*, T>>();
  for (const auto& block : blocks) {
    auto insn_in = bottom;
    for (Block* pred : block->preds()) {
      insn_in.meet(block_outs[pred->id()]);
    }
    for (auto it = block->begin(); it != block->end(); ++it) {
      if (it->type != MFLOW_OPCODE) {
        continue;
      }
      DexInstruction* insn = it->insn;
      insn_in_map->emplace(insn, insn_in);
      trans(insn, &insn_in);
    }
  }

  return insn_in_map;
}

template <typename T>
std::unique_ptr<std::unordered_map<DexInstruction*, T>> backwards_dataflow(
    const std::vector<Block*>& blocks,
    const T& bottom,
    const std::function<void(const DexInstruction*, T*)>& trans) {
  std::vector<T> block_ins(blocks.size(), bottom);
  std::deque<Block*> work_list(blocks.begin(), blocks.end());
  while (!work_list.empty()) {
    auto block = work_list.front();
    work_list.pop_front();
    auto insn_out = bottom;
    for (Block* succ : block->succs()) {
      insn_out.meet(block_ins[succ->id()]);
    }
    for (auto it = block->rbegin(); it != block->rend(); ++it) {
      if (it->type != MFLOW_OPCODE) {
        continue;
      }
      DexInstruction* insn = it->insn;
      trans(insn, &insn_out);
    }
    if (insn_out != block_ins[block->id()]) {
      block_ins[block->id()] = std::move(insn_out);
      for (auto pred : block->preds()) {
        if (std::find(work_list.begin(), work_list.end(), pred) ==
            work_list.end()) {
          work_list.push_back(pred);
        }
      }
    }
  }

  // Now we do a final pass and record the live-out at each instruction.  We
  // didn't record this information during the iterative analysis because we
  // would end up discarding all the information generated before the final
  // iteration, and it turns out that allocating and deallocating lots of
  // dynamic_bitsets is very expensive.
  auto insn_out_map =
      std::make_unique<std::unordered_map<DexInstruction*, T>>();
  for (const auto& block : blocks) {
    auto insn_out = bottom;
    for (Block* succ : block->succs()) {
      insn_out.meet(block_ins[succ->id()]);
    }
    for (auto it = block->rbegin(); it != block->rend(); ++it) {
      if (it->type != MFLOW_OPCODE) {
        continue;
      }
      DexInstruction* insn = it->insn;
      insn_out_map->emplace(insn, insn_out);
      trans(insn, &insn_out);
    }
  }

  return insn_out_map;
}
