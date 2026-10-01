/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "DexClass.h"
#include "RedexOptions.h"

namespace cfg {
class ControlFlowGraph;
} // namespace cfg

// Flags loops that the Android 10/11 ARMv7 JIT cannot compile.
//
// Each loop is scored on:
// - its merge block: the block in the loop that the most branches and gotos
//   jump to, where the loop's paths come back together;
// - M: how many branches and gotos jump to the merge block;
// - P: how many registers are written in the loop and still needed when
//   execution reaches the merge block.
// The Android 10/11 JIT emits one stack-to-stack copy per such register for
// each jump into the merge block, so the loop gets about P * M copies of ~8
// bytes each. Once the loop body passes 1,048,574 bytes, the backward
// conditional branch at its suspend check has no Thumb-2 encoding and VIXL
// aborts in UnimplementedDelegate. Android 12+ picks up VIXL's veneer for long
// backward branches through ART module updates; Android 10/11 cannot.
//
// Methods over 10,000 code units are skipped, as ART's default huge-method
// threshold does. A device can raise that threshold, but ART never compiles
// methods of 16,383 or more code units (Compiler::IsPathologicalCase).
class Armv7JitLoopChecker {
 public:
  struct LoopScore {
    // P.
    uint32_t carried_values{0};
    // M.
    uint32_t merge_in_degree{0};
    // carried_values * merge_in_degree, weighted up once the frame is large
    // enough that copies need longer address sequences.
    uint64_t score{0};
  };

  struct Weighting {
    // Carried values past which the frame exceeds 4 KB and copies to far slots
    // need extra address instructions.
    uint32_t large_frame_carried_values{380};
    // Past large_frame_carried_values, each further `divisor` carried values
    // add one more copy's worth of cost per copy. 0 disables the weighting.
    uint32_t large_frame_weight_divisor{210};
  };

  // A max score of 0 disables the check.
  Armv7JitLoopChecker(uint64_t max_score, const Weighting& weighting);

  // The crash is 32-bit ARM only. UNKNOWN counts, so that builds that do not
  // declare an architecture are not silently exempt.
  static bool can_crash(Architecture arch);

  // The highest-scoring loop in `cfg`, if the method has a loop. Computes the
  // exit block if liveness is needed.
  static std::optional<LoopScore> score_method(cfg::ControlFlowGraph& cfg,
                                               const Weighting& weighting);

  // Does not modify the IR: CFGs are built on copies.
  void run(const Scope& scope);

  bool fail() const { return !m_offenders.empty(); }

  // Methods at or above the max score, highest first.
  std::string print_offenders() const;

  uint64_t highest_score() const { return m_highest_score; }

 private:
  uint64_t m_max_score;
  Weighting m_weighting;
  uint64_t m_highest_score{0};
  std::vector<std::pair<uint64_t, std::string>> m_offenders;
};
