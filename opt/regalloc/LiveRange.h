/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <cstdint>
#include <boost/functional/hash.hpp>
#include <unordered_map>
#include <unordered_set>

#include "IRInstruction.h"

/*
 * This module renumbers registers so that they represent live ranges. Live
 * ranges are the union of use-def chains that share defs in common. See e.g.
 * Muchnick's Advanced Compiler Design & Implementation, Section 16.3.3 for
 * details.
 */

namespace regalloc {

namespace live_range {

using reg_t = uint16_t;

// Every IRInstruction has at most one def, so we can represent defs by
// instructions
using Def = IRInstruction*;

struct Use {
  IRInstruction* insn;
  reg_t reg;
  bool operator==(const Use&) const;
};

} // namespace live_range

} // namespace regalloc

namespace std {

template <>
struct hash<regalloc::live_range::Use> {
  size_t operator()(const regalloc::live_range::Use& use) const {
    size_t seed = boost::hash<IRInstruction*>()(use.insn);
    boost::hash_combine(seed, use.reg);
    return seed;
  }
};
}

namespace regalloc {

namespace live_range {

void renumber_registers(IRCode*);

} // namespace live_range

} // namespace regalloc
