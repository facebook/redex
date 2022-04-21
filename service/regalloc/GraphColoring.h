/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stack>

#include "ControlFlow.h"
#include "Interference.h"
#include "Liveness.h"
#include "Split.h"
#include "Transform.h"

namespace regalloc {

using vreg_t = uint16_t;

RangeSet init_range_set(cfg::ControlFlowGraph&);

namespace graph_coloring {

struct SpillPlan {
  // This is a map from symreg to the first available vreg when we tried to
  // allocate it. Basically a record of the failed attempts at register
  // coloring. Since different opcodes can address different maximum operand
  // sizes, we don't have to spill at every instruction -- just the ones that
  // have a maximum lower than our mapping.
  std::unordered_map<reg_t, vreg_t> global_spills;

  // Spills for param-related symbolic registers
  std::unordered_set<reg_t> param_spills;

  // Spills for range-instruction-related symbolic registers. The map's values
  // indicate the src indices that need to be spilled. We want to use the
  // indices rather than the src registers themselves because we don't want to
  // insert unnecessary spills when a register is used multiple times in a
  // given instruction. E.g. given
  //
  //   invoke-static (v0 v0 v1 v1 v2 v3) ...
  //
  // We may want to spill just the first occurrence of v0 or v1. If we used a
  // set of registers here (which we did previously), we would not be able to
  // represent that.
  std::unordered_map<const IRInstruction*, std::vector<size_t>> range_spills;

  bool empty() const {
    return global_spills.empty() && param_spills.empty() &&
           range_spills.empty();
  }
};

struct RegisterTransform {
  transform::RegMap map;
  // The size of the register frame. Note that we cannot simply walk the values
  // in the map to determine this; the size of the frame must be >= to the
  // largest virtual register in the map + its width.
  vreg_t size{0};
};

/*
 * This is a Chaitin-Briggs style allocator with some adaptations. See the
 * comment block of allocate() for details.
 *
 * The Allocator class exists solely to make it easy to track stats and read
 * from the config. All other state is passed around through method arguments.
 *
 * Relevant sources consulted when implementing this:
 *
 *  [Briggs92] P. Briggs. Register Allocation via Graph Coloring. PhD thesis,
 *    Rice University, 1992.
 *
 *  [Smith00] Michael D. Smith and Glenn Holloway. Graph-Coloring Register
 *    Allocation for Irregular Architectures. Technical report, Harvard
 *    University, 2000.
 */
class Allocator {

 public:
  struct Config {
    bool no_overwrite_this{false};
    bool use_splitting{false};
  };

  struct Stats {
    size_t reiteration_count{0};
    size_t param_spill_moves{0};
    size_t range_spill_moves{0};
    size_t global_spill_moves{0};
    size_t split_moves{0};
    size_t moves_coalesced{0};
    size_t params_spill_early{0};
    size_t moves_inserted() const {
      return param_spill_moves + range_spill_moves + global_spill_moves +
             split_moves;
    }
    size_t net_moves() const { return moves_inserted() - moves_coalesced; }
    Stats& operator+=(const Stats&);
  };

  Allocator() = default; // use default config

  explicit Allocator(const Config& config) : m_config(config) {}

  bool coalesce(interference::Graph*, cfg::ControlFlowGraph&);

  void simplify(interference::Graph*,
                std::stack<reg_t>* select_stack,
                std::stack<reg_t>* spilled_select_stack);

  void select(const cfg::ControlFlowGraph&,
              const interference::Graph&,
              std::stack<reg_t>* select_stack,
              RegisterTransform*,
              SpillPlan*);

  void select_ranges(const cfg::ControlFlowGraph&,
                     const interference::Graph&,
                     const RangeSet&,
                     RegisterTransform*,
                     SpillPlan*);

  void select_params(const cfg::ControlFlowGraph&,
                     const interference::Graph&,
                     RegisterTransform*,
                     SpillPlan*);

  void find_split(const interference::Graph&,
                  const SplitCosts&,
                  RegisterTransform*,
                  SpillPlan*,
                  SplitPlan*);

  std::unordered_map<reg_t, cfg::InstructionIterator> find_param_splits(
      const std::unordered_set<reg_t>&, cfg::ControlFlowGraph&);

  void split_params(const interference::Graph&,
                    const std::unordered_set<reg_t>& param_spills,
                    cfg::ControlFlowGraph&);

  void spill(const interference::Graph&,
             const SpillPlan&,
             const RangeSet&,
             cfg::ControlFlowGraph&);

  void allocate(cfg::ControlFlowGraph& cfg, bool);

  const Stats& get_stats() const { return m_stats; }

 private:
  Config m_config;
  Stats m_stats;
};

} // namespace graph_coloring

} // namespace regalloc
