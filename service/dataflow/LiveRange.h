/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/functional/hash.hpp>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "IRInstruction.h"

class IRCode;

/*
 * This module renumbers registers so that they represent live ranges. Live
 * ranges are the union of use-def chains that share defs in common. See e.g.
 * Muchnick's Advanced Compiler Design & Implementation, Section 16.3.3 for
 * details.
 */

namespace live_range {

// Every IRInstruction has at most one def, so we can represent defs by
// instructions
using Def = IRInstruction*;

struct Use {
  IRInstruction* insn;
  reg_t reg;
  bool operator==(const Use&) const;
};

} // namespace live_range

namespace std {

template <>
struct hash<live_range::Use> {
  size_t operator()(const live_range::Use& use) const {
    size_t seed = boost::hash<IRInstruction*>()(use.insn);
    boost::hash_combine(seed, use.reg);
    return seed;
  }
};

} // namespace std

namespace live_range {

/*
 * width_aware means that the renumbering process will allocate 2 slots per
 * wide register. In general, callers should use the default (true) value.
 */
void renumber_registers(IRCode*, bool width_aware = true);

} // namespace live_range
