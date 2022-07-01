/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/functional/hash.hpp>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "ControlFlow.h"
#include "IRInstruction.h"
#include "PatriciaTreeSet.h"
#include "ReachingDefinitions.h"

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
  src_index_t src_index;
  bool operator==(const Use&) const;
};

} // namespace live_range

namespace std {

template <>
struct hash<live_range::Use> {
  size_t operator()(const live_range::Use& use) const {
    size_t seed = boost::hash<IRInstruction*>()(use.insn);
    boost::hash_combine(seed, use.src_index);
    return seed;
  }
};

} // namespace std

namespace live_range {

using UseDefChains = std::unordered_map<Use, sparta::PatriciaTreeSet<Def>>;
using DefUseChains = std::unordered_map<Def, std::unordered_set<Use>>;

class Chains {
 public:
  explicit Chains(const cfg::ControlFlowGraph& cfg);
  UseDefChains get_use_def_chains() const;
  DefUseChains get_def_use_chains() const;

 private:
  const cfg::ControlFlowGraph& m_cfg;
  reaching_defs::FixpointIterator m_fp_iter;
};

class MoveAwareChains {
 public:
  explicit MoveAwareChains(const cfg::ControlFlowGraph& cfg);
  UseDefChains get_use_def_chains() const;
  DefUseChains get_def_use_chains() const;

 private:
  const cfg::ControlFlowGraph& m_cfg;
  reaching_defs::MoveAwareFixpointIterator m_fp_iter;
};

/*
 * width_aware means that the renumbering process will allocate 2 slots per
 * wide register. In general, callers should use the default (true) value.
 */
void renumber_registers(IRCode*, bool width_aware = true);

} // namespace live_range
