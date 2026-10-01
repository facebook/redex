/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Armv7JitLoopChecker.h"

#include <algorithm>
#include <functional>

#include "ControlFlow.h"
#include "IRCode.h"
#include "Liveness.h"
#include "LoopInfo.h"
#include "Show.h"
#include "Walkers.h"

namespace {

// ART skips methods above kDefaultHugeMethodThreshold code units, in the JIT as
// well as in dex2oat (HGraphBuilder::SkipCompilation).
constexpr uint32_t kArtHugeMethodThreshold = 10000;

using LoopScore = Armv7JitLoopChecker::LoopScore;
using Weighting = Armv7JitLoopChecker::Weighting;

uint64_t weighted_score(uint32_t carried_values,
                        uint32_t merge_in_degree,
                        const Weighting& weighting) {
  uint64_t copies = uint64_t{carried_values} * merge_in_degree;
  if (carried_values <= weighting.large_frame_carried_values ||
      weighting.large_frame_weight_divisor == 0) {
    return copies;
  }
  return copies + copies *
                      (carried_values - weighting.large_frame_carried_values) /
                      weighting.large_frame_weight_divisor;
}

uint32_t normal_in_degree(const cfg::Block& block) {
  return static_cast<uint32_t>(std::count_if(
      block.preds().begin(), block.preds().end(), [](const cfg::Edge* e) {
        return e->type() == cfg::EDGE_GOTO || e->type() == cfg::EDGE_BRANCH;
      }));
}

// A loop needs a backward branch. In linear IR a branch target that appears
// before its branch is one, so most methods can be skipped without a CFG.
bool has_backward_branch(const IRCode& code) {
  UnorderedSet<const MethodItemEntry*> seen_branches;
  for (const auto& mie : code) {
    if (mie.type == MFLOW_OPCODE) {
      seen_branches.insert(&mie);
    } else if (mie.type == MFLOW_TARGET &&
               !seen_branches.contains(mie.target->src)) {
      return true;
    }
  }
  return false;
}

std::string describe(const DexMethod* method, const LoopScore& score) {
  return show(method) +
         " (carried_values=" + std::to_string(score.carried_values) +
         ", merge_in_degree=" + std::to_string(score.merge_in_degree) +
         ", score=" + std::to_string(score.score) + ")";
}

struct Stats {
  uint64_t highest_score{0};
  std::vector<std::pair<uint64_t, std::string>> offenders;

  Stats& operator+=(const Stats& other) {
    highest_score = std::max(highest_score, other.highest_score);
    offenders.insert(offenders.end(), other.offenders.begin(),
                     other.offenders.end());
    return *this;
  }
};

} // namespace

Armv7JitLoopChecker::Armv7JitLoopChecker(uint64_t max_score,
                                         const Weighting& weighting)
    : m_max_score(max_score), m_weighting(weighting) {}

bool Armv7JitLoopChecker::can_crash(Architecture arch) {
  return arch == Architecture::ARMV7 || arch == Architecture::ARM ||
         arch == Architecture::UNKNOWN;
}

std::optional<LoopScore> Armv7JitLoopChecker::score_method(
    cfg::ControlFlowGraph& cfg, const Weighting& weighting) {
  loop_impl::LoopInfo loops(cfg);
  if (loops.num_loops() == 0) {
    return std::nullopt;
  }

  std::optional<LivenessFixpointIterator> liveness;
  std::optional<LoopScore> best;
  for (auto& loop : loops) {
    cfg::Block* merge = nullptr;
    uint32_t merge_in_degree = 0;
    for (auto* block : loop) {
      auto in_degree = normal_in_degree(*block);
      if (in_degree > merge_in_degree) {
        merge_in_degree = in_degree;
        merge = block;
      }
    }
    // A carried value needs a register, so the register count bounds P.
    if (merge == nullptr ||
        (best && weighted_score(cfg.get_registers_size(), merge_in_degree,
                                weighting) <= best->score)) {
      continue;
    }

    if (!liveness) {
      cfg.calculate_exit_block();
      liveness.emplace(cfg);
      liveness->run(LivenessDomain());
    }
    const auto& live_in = liveness->get_live_in_vars_at(merge);
    UnorderedSet<reg_t> carried;
    for (auto* block : loop) {
      for (const auto& mie : InstructionIterable(block)) {
        if (mie.insn->has_dest() && live_in.contains(mie.insn->dest())) {
          carried.insert(mie.insn->dest());
        }
      }
    }

    auto carried_values = static_cast<uint32_t>(carried.size());
    LoopScore score{carried_values, merge_in_degree,
                    weighted_score(carried_values, merge_in_degree, weighting)};
    if (!best || score.score > best->score) {
      best = score;
    }
  }
  return best;
}

void Armv7JitLoopChecker::run(const Scope& scope) {
  if (m_max_score == 0) {
    return;
  }
  auto stats =
      walk::parallel::methods<Stats>(scope, [&](DexMethod* method, Stats* acc) {
        const auto* code = method->get_code();
        if (code == nullptr ||
            code->estimate_code_units() > kArtHugeMethodThreshold ||
            (!code->cfg_built() && !has_backward_branch(*code))) {
          return;
        }
        // Building and clearing a CFG could re-linearize the IR differently.
        IRCode copy(*code);
        if (!copy.cfg_built()) {
          copy.build_cfg();
        }
        auto& cfg = copy.cfg();
        if (cfg.estimate_code_units() + cfg.get_size_adjustment() >
            kArtHugeMethodThreshold) {
          return;
        }
        auto score = score_method(cfg, m_weighting);
        if (!score) {
          return;
        }
        acc->highest_score = std::max(acc->highest_score, score->score);
        if (score->score >= m_max_score) {
          acc->offenders.emplace_back(score->score, describe(method, *score));
        }
      });
  m_highest_score = stats.highest_score;
  m_offenders = std::move(stats.offenders);
  std::sort(m_offenders.begin(), m_offenders.end(), std::greater<>());
}

std::string Armv7JitLoopChecker::print_offenders() const {
  std::string out = std::to_string(m_offenders.size()) +
                    " method(s) would abort the Android 10/11 ARMv7 JIT "
                    "(armv7_jit_loop_max_score=" +
                    std::to_string(m_max_score) + "):";
  for (const auto& [_, description] : m_offenders) {
    out += "\n  " + description;
  }
  return out;
}
