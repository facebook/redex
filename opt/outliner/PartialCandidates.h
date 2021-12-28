/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <utility>
#include <vector>

#include "ControlFlow.h"
#include "IRInstruction.h"

namespace outliner_impl {

enum class RegState : uint8_t {
  // Incompatible assignments imply that this defined register cannot possibly
  // be live-out
  INCONSISTENT,
  // A newly created object on which no constructor was invoked yet
  UNINITIALIZED,
  // A primitive value, array, or object on which a constructor was invoked
  INITIALIZED,
};

struct DefinedReg {
  bool wide;
  RegState state;
};

struct PartialCandidateNode {
  std::vector<IRInstruction*> insns;
  std::unordered_map<reg_t, DefinedReg> defined_regs;
  std::vector<std::pair<cfg::Edge*, std::shared_ptr<PartialCandidateNode>>>
      succs;
};

// A partial candidate is still evolving, and defined against actual
// instructions that have not been normalized yet.
struct PartialCandidate {
  std::unordered_set<reg_t> in_regs;
  PartialCandidateNode root;
  // Total number of all instructions
  size_t insns_size{0};
  // Approximate number of code units occupied by all instructions
  size_t size{0};
  // Number of temporary registers needed hold all the defined regs
  reg_t temp_regs{0};
};

} // namespace outliner_impl
