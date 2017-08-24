/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <stack>

#include "Interference.h"
#include "IRCode.h"
#include "Liveness.h"
#include "Split.h"
#include "Transform.h"

namespace regalloc {

using reg_t = uint16_t;

RangeSet init_range_set(IRCode*);

namespace graph_coloring {

struct SpillPlan {
  // This is a map from symreg to the first available vreg when we tried to
  // allocate it. Basically a record of the failed attempts at register
  // coloring. Since different opcodes can address different maximum operand
  // sizes, we don't have to spill at every instruction -- just the ones that
  // have a maximum lower than our mapping.
  std::unordered_map<reg_t, reg_t> global_spills;

  // Spills for param-related symbolic registers
  std::unordered_set<reg_t> param_spills;

  // Spills for range-instruction-related symbolic registers
  std::unordered_map<const IRInstruction*, std::unordered_set<reg_t>>
      range_spills;

  std::unordered_map<reg_t, size_t> spill_costs;

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
  uint16_t size{0};
};

/*
 * This is a Chaitin-Briggs style allocator with some adaptations. See the
 * comment block of allocate() for details.
 *
 * The Allocator class exists solely to make it easy to track stats. All other
 * state is passed around through method arguments.
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
    void accumulate(const Stats&);
  };

  bool coalesce(interference::Graph*, IRCode*);

  void simplify(bool select_spill_later,
                interference::Graph*,
                std::stack<reg_t>* select_stack,
                std::stack<reg_t>* spilled_select_stack);

  void select(const IRCode*,
              const interference::Graph&,
              std::stack<reg_t>* select_stack,
              RegisterTransform*,
              SpillPlan*);

  void choose_range_promotions(const IRCode*,
                               const interference::Graph&,
                               const SpillPlan&,
                               RangeSet*);

  void select_ranges(const IRCode*,
                     const interference::Graph&,
                     const RangeSet&,
                     RegisterTransform*,
                     SpillPlan*);

  void select_params(const IRCode*,
                     const interference::Graph&,
                     RegisterTransform*,
                     SpillPlan*);

  void spill_costs(const IRCode*,
                   const interference::Graph&,
                   const RangeSet&,
                   SpillPlan*);

  void find_split(const interference::Graph&,
                  const SplitCosts&,
                  RegisterTransform*,
                  SpillPlan*,
                  SplitPlan*);

  std::unordered_map<reg_t, FatMethod::iterator> find_param_first_uses(
      const std::unordered_set<reg_t>&, bool spill_param_properly, IRCode*);

  void spill_params(const interference::Graph&,
                    const std::unordered_map<reg_t, FatMethod::iterator>&,
                    IRCode*,
                    std::unordered_set<reg_t>*);

  void spill(const interference::Graph&,
             const SpillPlan&,
             const RangeSet&,
             IRCode*,
             std::unordered_set<reg_t>*);

  void allocate(bool use_splitting,
                bool spill_param_properly,
                bool select_spill_later,
                IRCode*);

  const Stats& get_stats() const { return m_stats; }

 private:
  Stats m_stats;
};

} // namespace graph_coloring

} // namespace regalloc
