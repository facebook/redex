/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReduceSparseSwitchesPass.h"

#include <filesystem>
#include <fstream>
#include <system_error>

#include "ConfigFiles.h"
#include "DexInstruction.h"
#include "InstructionLowering.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "StlUtil.h"
#include "Trace.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_AFFECTED_METHODS = "num_affected_methods";
constexpr const char* METRIC_REMOVED_TRIVIAL_SWITCH_CASES =
    "num_removed_trivial_switch_cases";
constexpr const char* METRIC_REMOVED_TRIVIAL_SWITCHES =
    "num_removed_trivial_switches";
constexpr const char* METRIC_SPLITTING_TRANSFORMATIONS =
    "num_splitting_transformations";
constexpr const char* METRIC_SPLITTING_TRANSFORMATIONS_PACKED_SEGMENTS =
    "num_splitting_transformations_packed_segments";
constexpr const char* METRIC_SPLITTING_TRANSFORMATIONS_SWITCH_CASES_PACKED =
    "num_splitting_transformations_switch_cases_packed";
constexpr const char* METRIC_MULTIPLEXING_ABANDONED_PREFIX =
    "num_multiplexing_abandoned_";
constexpr const char* METRIC_MULTIPLEXING_TRANSFORMATIONS =
    "num_multiplexing_transformations";
constexpr const char*
    METRIC_MULTIPLEXING_TRANSFORMATIONS_AVERAGE_INEFFICIENCY_PREFIX =
        "num_multiplexing_transformations_average_inefficiency_";
constexpr const char* METRIC_MULTIPLEXING_TRANSFORMATIONS_SWITCH_CASES =
    "num_multiplexing_transformations_switch_cases";
constexpr const char* METRIC_EXPANDED_TRANSFORMATIONS =
    "num_expanded_transformations";
constexpr const char* METRIC_EXPANDED_SWITCH_CASES =
    "num_expanded_switch_cases";

bool is_sufficiently_sparse(cfg::Block* block) {
  always_assert(opcode::is_switch(block->get_last_insn()->insn->opcode()));
  instruction_lowering::CaseKeysExtentBuilder ckeb;
  for (auto* e : block->succs()) {
    if (e->type() == cfg::EDGE_BRANCH) {
      ckeb.insert(*e->case_key());
    }
  }
  return ckeb->sufficiently_sparse();
}

struct Segment {
  int64_t first;
  int64_t last;
  Segment(int64_t first, int64_t last) : first(first), last(last) {}
  size_t size() const { return last - first + 1; }
};

static void split_sparse_switch_into_packed_and_sparse(
    cfg::ControlFlowGraph& cfg,
    cfg::Block* block,
    IRList::iterator switch_insn_it,
    const std::vector<int32_t>& case_keys,
    const std::vector<Segment>& packed_segments) {
  // We now rewrite the switch from
  //   /* sparse */ switch (selector) {
  //     case K_0:          goto B_0;
  //       ...
  //     case K_{first-1}:  goto B_{first-1};
  //     case K_{first}:    goto B_{first};
  //       ...
  //     case K_{last}:     goto B_{last};
  //     case K_{last+1}:   goto B_{last+1};
  //       ...
  //     case K_{N-1}:      goto B_{N-1};
  //     default:           goto B_{default};
  //   }
  // to
  //   /* packed */ switch (selector) {
  //     K_{first}:             goto B_{first};
  //     ...
  //     K_{last}:              goto B_{last};
  //     default:
  //       /* usually sparse, but possibly packed */ switch (selector) {
  //         case K_0:          goto B_0;
  //           ...
  //         case K_{first-1}:  goto B_{first-1};
  //         case K_{last+1}:   goto B_{last+1};
  //           ...
  //         case K_{N-1}:      goto B_{N-1};
  //         default:           goto B_{default};
  //       }
  //   }

  for (auto [first, last] : packed_segments) {
    auto selector_reg = switch_insn_it->insn->src(0);
    auto* goto_block = block->goes_to();
    auto first_case_key = case_keys[first];
    auto last_case_key = case_keys[last];
    std::vector<std::pair<int32_t, cfg::Block*>> secondary_switch_case_to_block;
    std::vector<cfg::Edge*> sparse_edges;
    cfg::Edge* default_edge = nullptr;
    for (auto* e : block->succs()) {
      if (e->type() == cfg::EDGE_GOTO) {
        default_edge = e;
        continue;
      }
      always_assert(e->type() == cfg::EDGE_BRANCH);
      auto case_key = *e->case_key();
      if (case_key >= first_case_key && case_key <= last_case_key) {
        continue;
      }
      secondary_switch_case_to_block.emplace_back(case_key, e->target());
      sparse_edges.push_back(e);
    }
    if (secondary_switch_case_to_block.empty()) {
      // The last switch is including all remaining case keys, so there's
      // nothing to rewrite.
      break;
    }
    auto* secondary_switch_block = cfg.create_block();
    always_assert(default_edge != nullptr);
    cfg.set_edge_target(default_edge, secondary_switch_block);
    cfg.delete_edges(sparse_edges.begin(), sparse_edges.end());
    cfg.create_branch(
        secondary_switch_block,
        (new IRInstruction(OPCODE_SWITCH))->set_src(0, selector_reg),
        goto_block, secondary_switch_case_to_block);
    always_assert(!is_sufficiently_sparse(block));

    block = secondary_switch_block;
    switch_insn_it = block->get_last_insn();
  }
}

static void multiplex_sparse_switch_into_packed_and_sparse(
    cfg::ControlFlowGraph& cfg,
    cfg::Block* block,
    const IRList::iterator& switch_insn_it,
    reg_t tmp_reg,
    int32_t shr_by,
    const std::vector<std::vector<cfg::Edge*>>& multiplexed_cases) {
  // For some suitable M, a power of 2, and shr_by, we rewrite the switch from
  //   /* sparse */ switch (selector) {
  //       ...
  //     case K_i:       goto B_i;
  //       ...
  //     default:        goto B_{default};
  //   }
  // to
  //   /* packed */ switch ((selector >> shr_by) & (M-1)) {
  //     0:              /* usually sparse */ switch (selector) {
  //                         ...
  //                       case K_x: // where (K_x >> shr_by) & (M - 1) == 0
  //                                 goto B_x;
  //                         ...
  //                       default: goto B_{default};
  //                     }
  //       ...
  //     M-1:           /* usually sparse */ switch (selector) {
  //                         ...
  //                       case K_x: // where (K_y >> shr_by) & (M - 1) == M - 1
  //                                 goto B_y;
  //                         ...
  //                       default: goto B_{default};
  //                     }
  //     default:        goto B_{default};
  //   }

  auto* template_sb =
      source_blocks::get_last_source_block_before(block, switch_insn_it);
  auto M = multiplexed_cases.size();
  auto selector_reg = switch_insn_it->insn->src(0);
  auto* goto_block = block->goes_to();
  std::vector<std::pair<int32_t, cfg::Block*>> packed_cases;
  packed_cases.reserve(M);
  auto get_inner_block = [&](auto& cases) {
    if (cases.empty()) {
      return goto_block;
    }
    auto* block = cfg.create_block();
    if (template_sb) {
      auto new_sb = source_blocks::clone_as_synthetic(template_sb);
      block->insert_before(block->end(), std::move(new_sb));
    }
    if (cases.size() == 1) {
      auto edge = cases.front();
      block->push_back((new IRInstruction(OPCODE_CONST))
                           ->set_dest(tmp_reg)
                           ->set_literal(*edge->case_key()));
      cfg.create_branch(block,
                        (new IRInstruction(OPCODE_IF_NE))
                            ->set_src(0, selector_reg)
                            ->set_src(1, tmp_reg),
                        edge->target(),
                        goto_block);
      return block;
    }
    std::vector<std::pair<int32_t, cfg::Block*>> sparse_cases;
    sparse_cases.reserve(cases.size());
    for (auto* e : cases) {
      sparse_cases.emplace_back(*e->case_key(), e->target());
    }
    cfg.create_branch(
        block, (new IRInstruction(OPCODE_SWITCH))->set_src(0, selector_reg),
        goto_block, sparse_cases);
    return block;
  };
  for (auto& cases : multiplexed_cases) {
    packed_cases.emplace_back(static_cast<int32_t>(packed_cases.size()),
                              get_inner_block(cases));
  }

  cfg.remove_insn(block->to_cfg_instruction_iterator(switch_insn_it));
  reg_t shred_reg = selector_reg;
  if (shr_by > 0) {
    shred_reg = tmp_reg;
    block->push_back((new IRInstruction(OPCODE_SHR_INT_LIT))
                         ->set_dest(shred_reg)
                         ->set_src(0, selector_reg)
                         ->set_literal(shr_by));
  }
  block->push_back((new IRInstruction(OPCODE_AND_INT_LIT))
                       ->set_dest(tmp_reg)
                       ->set_src(0, shred_reg)
                       ->set_literal(M - 1));
  cfg.create_branch(block,
                    (new IRInstruction(OPCODE_SWITCH))->set_src(0, tmp_reg),
                    goto_block, packed_cases);
}

static bool fits_16(int32_t v) { return v >= -32768 && v < 32768; }

static void expand_switch(
    cfg::ControlFlowGraph& cfg,
    cfg::Block* block,
    reg_t tmp_reg,
    const IRList::iterator& switch_insn_it,
    const std::vector<std::pair<int32_t, cfg::Block*>>& cases,
    cfg::Block* default_target) {
  auto selector_reg = switch_insn_it->insn->src(0);
  cfg.remove_insn(block->to_cfg_instruction_iterator(switch_insn_it));

  std::optional<int32_t> prev_case_key;
  for (size_t i = 0; i < cases.size(); i++) {
    auto [case_key, target] = cases[i];
    cfg::Block* next_block =
        i == cases.size() - 1 ? default_target : cfg.create_block();
    IRInstruction* if_insn;
    if (case_key == 0) {
      if_insn = new IRInstruction(OPCODE_IF_EQZ);
    } else {
      IRInstruction* init_insn = [&, case_key = case_key] {
        if (prev_case_key && !fits_16(case_key)) {
          int32_t diff = case_key - *prev_case_key;
          if (fits_16(diff)) {
            return (new IRInstruction(OPCODE_ADD_INT_LIT))
                ->set_dest(tmp_reg)
                ->set_src(0, tmp_reg)
                ->set_literal(diff);
          }
        }
        return (new IRInstruction(OPCODE_CONST))
            ->set_dest(tmp_reg)
            ->set_literal(case_key);
      }();
      block->push_back(init_insn);
      if_insn = (new IRInstruction(OPCODE_IF_EQ))->set_src(1, tmp_reg);
    }
    if_insn->set_src(0, selector_reg);
    cfg.create_branch(block, if_insn, next_block, target);
    block = next_block;
    prev_case_key = case_key;
  }
}

void write_sparse_switches(DexStoresVector& stores,
                           ConfigFiles& conf,
                           uint64_t threshold) {
  std::filesystem::path dirpath(conf.metafile("sparse_switches"));
  std::error_code ec;
  std::filesystem::create_directories(dirpath, ec);
  always_assert(ec.value() == 0);

  walk::parallel::methods(build_class_scope(stores), [&](DexMethod* method) {
    if (method->get_code() == nullptr) {
      return;
    }
    cfg::ScopedCFG cfg(method->get_code());
    size_t running_index = 0;
    for (auto* block : cfg->blocks()) {
      auto last_insn_it = block->get_last_insn();
      if (last_insn_it == block->end()) {
        continue;
      }
      if (!opcode::is_switch(last_insn_it->insn->opcode())) {
        continue;
      }
      if (block->succs().size() - 1 < threshold) {
        continue;
      }

      if (!is_sufficiently_sparse(block)) {
        continue;
      }

      auto method_name = show_deobfuscated(method);
      std::replace(method_name.begin(), method_name.end(), '/', '.');
      method_name.append(".");
      method_name.append(std::to_string(running_index));
      method_name.append(".csv");
      ++running_index;

      auto name = dirpath / method_name;

      std::ofstream file(name);
      for (auto* e : block->succs()) {
        if (e->type() == cfg::EDGE_BRANCH) {
          file << *e->case_key() << "\n";
        }
      }
    }
  });
}

bool partition(const std::vector<int32_t>& case_keys,
               size_t min_switch_cases_per_segment,
               std::vector<Segment>* packed_segments,
               std::vector<int32_t>* sparse_case_keys) {
  // We start by treating each case key as a separate segment. Then we'll
  // iteratively marge adjacent segments that together are packed. Whenever we
  // merge, we look to the left if there's an adjacent segment that is now
  // mergable, and if not, we keep going to the right. Eventually, all mergable
  // segments will have been merged. This algorithm is linear in the number of
  // case keys (only the later packed segment sorting is obviously not).
  packed_segments->reserve(case_keys.size());
  auto back = [v = packed_segments]() -> Segment& { return v->back(); };
  auto back2 = [v = packed_segments]() -> Segment& { return v->end()[-2]; };
  for (size_t i = 0; i < case_keys.size(); ++i) {
    packed_segments->emplace_back(i, i);
    // Iteratively fuse last two segments until no longer possible.
    while (packed_segments->size() >= 2 &&
           !instruction_lowering::CaseKeysExtent{
               case_keys[back2().first], case_keys[back().last],
               (uint32_t)(back().last - back2().first + 1)}
                .sufficiently_sparse()) {
      // Fuse last two segments.
      back2().last = back().last;
      packed_segments->pop_back();
    }
  }

  // We now scan all remaining segments and identify which ones are trivial
  // segments, and move those over to the sparse case keys collection.
  std20::erase_if(*packed_segments, [&](const auto& segment) {
    if (segment.first != segment.last) {
      return false;
    }
    sparse_case_keys->push_back(case_keys[segment.first]);
    return true;
  });

  // Make it so that the largest packed segments come first to reduce average
  // runtime cost, assuming that all case-keys get selected with the same
  // frequency.
  std::sort(packed_segments->begin(), packed_segments->end(),
            [](const auto& a, const auto& b) {
              if (a.size() != b.size()) {
                return a.size() > b.size();
              }
              return a.first < b.first;
            });

  // We move unproductive (too small) packed segments over to the remaining
  // sparse keys.
  auto unpack_last_segment = [&]() {
    const auto& segment = packed_segments->back();
    for (int64_t i = segment.first; i <= segment.last; i++) {
      sparse_case_keys->push_back(case_keys[i]);
    }
    packed_segments->pop_back();
  };
  while (!packed_segments->empty() &&
         packed_segments->back().size() < sparse_case_keys->size()) {
    unpack_last_segment();
  }

  double partitioned_log2_cost = packed_segments->size();
  if (!sparse_case_keys->empty()) {
    partitioned_log2_cost += std::log2(sparse_case_keys->size());
  }
  if (partitioned_log2_cost > std::log2(case_keys.size())) {
    return false;
  }

  // Okay, so it's conceptually worthwhile doing. We still want to avoid too
  // small packed segments for practical reason.
  while (!packed_segments->empty() &&
         packed_segments->back().size() < min_switch_cases_per_segment) {
    unpack_last_segment();
  }

  return !packed_segments->empty();
}
} // namespace

// Find switches which can be split into packed and sparse switches, and apply
// the transformation.
ReduceSparseSwitchesPass::Stats
ReduceSparseSwitchesPass::trivial_transformation(cfg::ControlFlowGraph& cfg) {
  ReduceSparseSwitchesPass::Stats stats;
  for (auto* block : cfg.blocks()) {
    auto last_insn_it = block->get_last_insn();
    if (last_insn_it == block->end()) {
      continue;
    }
    if (!opcode::is_switch(last_insn_it->insn->opcode())) {
      continue;
    }
    std::vector<cfg::Edge*> trivial_edges;
    auto* default_target = block->goes_to();
    for (auto* e : block->succs()) {
      if (e->type() == cfg::EDGE_BRANCH && e->target() == default_target) {
        trivial_edges.push_back(e);
      }
    }
    cfg.delete_edges(trivial_edges.begin(), trivial_edges.end());
    stats.removed_trivial_switch_cases += trivial_edges.size();
    if (block->succs().size() == 1) {
      stats.removed_trivial_switches++;
      continue;
    }
  }
  return stats;
}

// Find switches which can be split into packed and sparse switches, and apply
// the transformation.
ReduceSparseSwitchesPass::Stats
ReduceSparseSwitchesPass::splitting_transformation(
    size_t min_switch_cases,
    size_t min_switch_cases_per_segment,
    cfg::ControlFlowGraph& cfg) {
  always_assert(min_switch_cases > 0);
  ReduceSparseSwitchesPass::Stats stats;
  for (auto* block : cfg.blocks()) {
    auto last_insn_it = block->get_last_insn();
    if (last_insn_it == block->end()) {
      continue;
    }
    if (!opcode::is_switch(last_insn_it->insn->opcode())) {
      continue;
    }
    if (block->succs().size() - 1 < min_switch_cases) {
      continue;
    }
    if (!is_sufficiently_sparse(block)) {
      continue;
    }

    // The (ordered) switch case keys are a (monotonically) increasing sequence
    // of numbers K_0, ..., K_{N-1} (where N is the number of switch cases). We
    // try to find a maximal subsequence K_{first}, ..., K_{last} such that a
    // switch with these numbers is not sparse.
    std::vector<int32_t> case_keys;
    case_keys.reserve(block->succs().size() - 1);
    for (auto* e : block->succs()) {
      if (e->type() == cfg::EDGE_BRANCH) {
        case_keys.push_back(*e->case_key());
      }
    }
    always_assert(case_keys.size() + 1 == block->succs().size());
    always_assert(!case_keys.empty());
    std::sort(case_keys.begin(), case_keys.end());

    std::vector<Segment> packed_segments;
    std::vector<int32_t> sparse_case_keys;

    if (!partition(case_keys, min_switch_cases_per_segment, &packed_segments,
                   &sparse_case_keys)) {
      continue;
    }

    split_sparse_switch_into_packed_and_sparse(cfg, block, last_insn_it,
                                               case_keys, packed_segments);

    stats.splitting_transformations++;
    stats.splitting_transformations_packed_segments += packed_segments.size();
    stats.splitting_transformations_switch_cases_packed +=
        case_keys.size() - sparse_case_keys.size();
  }
  return stats;
}

// Find switches which can be multiplexed into packed and sparse switches, and
// apply the transformation.
ReduceSparseSwitchesPass::Stats
ReduceSparseSwitchesPass::multiplexing_transformation(
    size_t min_switch_cases, cfg::ControlFlowGraph& cfg) {
  always_assert(min_switch_cases > 0);
  ReduceSparseSwitchesPass::Stats stats;
  std::optional<reg_t> tmp_reg;
  for (auto* block : cfg.blocks()) {
    auto last_insn_it = block->get_last_insn();
    if (last_insn_it == block->end()) {
      continue;
    }
    if (!opcode::is_switch(last_insn_it->insn->opcode())) {
      continue;
    }
    auto switch_cases = block->succs().size() - 1;
    if (switch_cases < min_switch_cases) {
      continue;
    }
    if (!is_sufficiently_sparse(block)) {
      continue;
    }

    always_assert(switch_cases > 0);
    // For the number of buckets, we choose the square root of the switch cases,
    // rounded up to the next power of 2.
    size_t M =
        1 << static_cast<size_t>(std::ceil(std::log2(std::sqrt(switch_cases))));
    always_assert(M > 0);
    always_assert(M <= 65536);
    int32_t shr_by = -1;
    size_t max_cases = std::numeric_limits<size_t>::max();
    size_t L = 31 - static_cast<size_t>(std::log2(M));
    always_assert(L > 0);
    for (size_t i = 0; i < L; i++) {
      std::vector<size_t> multiplexed_cases(M);
      for (auto* e : block->succs()) {
        if (e->type() == cfg::EDGE_BRANCH) {
          auto j = static_cast<uint32_t>((*e->case_key()) >> i) & (M - 1);
          multiplexed_cases[j]++;
        }
      }
      auto max =
          *std::max_element(multiplexed_cases.begin(), multiplexed_cases.end());
      if (max < max_cases) {
        max_cases = max;
        shr_by = i;
      }
    }
    always_assert(shr_by >= 0);
    bool abandon = max_cases > switch_cases / 2;
    TRACE(RSS, 4,
          "Sparse switch with %zu cases >> %d %% %zu ==> %zu max; abandon: %d",
          switch_cases, shr_by, M, max_cases, abandon);
    if (abandon) {
      stats.multiplexing[M].abandoned++;
      continue;
    }

    std::vector<std::vector<cfg::Edge*>> multiplexed_cases(M);
    for (auto* e : block->succs()) {
      if (e->type() == cfg::EDGE_BRANCH) {
        auto j = static_cast<uint32_t>((*e->case_key()) >> shr_by) & (M - 1);
        multiplexed_cases[j].push_back(e);
      }
    }

    if (!tmp_reg) {
      tmp_reg = cfg.allocate_temp();
    }
    multiplex_sparse_switch_into_packed_and_sparse(
        cfg, block, last_insn_it, *tmp_reg, shr_by, multiplexed_cases);

    stats.multiplexing[M].transformations++;
    stats.multiplexing[M].switch_cases += switch_cases;
    always_assert(M * max_cases >= switch_cases);
    stats.multiplexing[M].inefficiency +=
        static_cast<size_t>(std::log2(M * max_cases / switch_cases));
  }
  return stats;
}

// Expanding remaining sparse switches, and also very small packed switches.
ReduceSparseSwitchesPass::Stats ReduceSparseSwitchesPass::expand_transformation(
    cfg::ControlFlowGraph& cfg) {
  ReduceSparseSwitchesPass::Stats stats;
  std::optional<reg_t> tmp_reg;
  for (auto* block : cfg.blocks()) {
    auto last_insn_it = block->get_last_insn();
    if (last_insn_it == block->end()) {
      continue;
    }
    if (!opcode::is_switch(last_insn_it->insn->opcode())) {
      continue;
    }
    bool sparse = is_sufficiently_sparse(block);
    auto switch_cases = block->succs().size() - 1;
    if (!sparse && switch_cases > 6) {
      // It's never worth expanding a large packed switch.
      continue;
    }
    std::vector<std::pair<int32_t, cfg::Block*>> cases;
    cfg::Block* default_target = nullptr;
    for (auto* e : block->succs()) {
      if (e->type() == cfg::EDGE_GOTO) {
        default_target = e->target();
        continue;
      }
      always_assert(e->type() == cfg::EDGE_BRANCH);
      auto case_key = *e->case_key();
      auto* target = e->target();
      cases.emplace_back(case_key, target);
    }
    always_assert(default_target != nullptr);
    always_assert(!cases.empty());

    // TODO: Consider sorting cases by target-hotness (for speed)
    std::sort(cases.begin(), cases.end(),
              [&](auto& p, auto& q) { return p.first < q.first; });
    uint32_t original_size = (sparse ? 5 : 7) + (2 + 2 * sparse) * cases.size();
    uint32_t expanded_size = 0;
    std::optional<int32_t> prev_case_key;
    for (auto [case_key, _] : cases) {
      if (case_key >= -8 && case_key < 8) {
        expanded_size += 3;
      } else if (!fits_16(case_key) || !prev_case_key ||
                 !fits_16(case_key - *prev_case_key)) {
        expanded_size += 5;
      } else {
        expanded_size += 4;
      }
      prev_case_key = case_key;
    }
    if (expanded_size >= original_size) {
      // Nothing to gain by exploding instructions
      continue;
    }

    if (!tmp_reg) {
      tmp_reg = cfg.allocate_temp();
    }
    expand_switch(cfg, block, *tmp_reg, last_insn_it, cases, default_target);
    stats.expanded_transformations++;
    stats.expanded_switch_cases += switch_cases;
  }
  return stats;
}

void ReduceSparseSwitchesPass::bind_config() {
  bind("min_splitting_switch_cases",
       m_config.min_splitting_switch_cases,
       m_config.min_splitting_switch_cases);

  bind("min_splitting_switch_cases_per_segment",
       m_config.min_splitting_switch_cases_per_segment,
       m_config.min_splitting_switch_cases_per_segment);

  bind("min_multiplexing_switch_cases",
       m_config.min_multiplexing_switch_cases,
       m_config.min_multiplexing_switch_cases);

  bind("expand_remaining", m_config.expand_remaining,
       m_config.expand_remaining);
  bind("write_sparse_switches", m_config.write_sparse_switches,
       m_config.write_sparse_switches);
};

void ReduceSparseSwitchesPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& conf,
                                        PassManager& mgr) {
  if (m_config.write_sparse_switches < std::numeric_limits<uint64_t>::max()) {
    write_sparse_switches(stores, conf, m_config.write_sparse_switches);
  }

  // Don't run under instrumentation.
  if (mgr.get_redex_options().instrument_pass_enabled) {
    return;
  }

  auto scope = build_class_scope(stores);

  Stats stats;
  std::mutex stats_mutex;
  walk::parallel::code(scope, [&](const DexMethod* method, IRCode& code) {
    if (method->rstate.no_optimizations() ||
        method->rstate.should_not_outline()) {
      return;
    }

    auto& cfg = code.cfg();
    auto local_stats = trivial_transformation(cfg);

    local_stats += splitting_transformation(
        m_config.min_splitting_switch_cases,
        m_config.min_splitting_switch_cases_per_segment, cfg);

    local_stats += multiplexing_transformation(
        m_config.min_multiplexing_switch_cases, cfg);

    if (m_config.expand_remaining) {
      local_stats += expand_transformation(cfg);
    }

    if (local_stats.removed_trivial_switch_cases == 0 &&
        local_stats.splitting_transformations == 0 &&
        local_stats.multiplexing_transformations() == 0 &&
        local_stats.expanded_transformations == 0) {
      return;
    }

    TRACE(RSS, 3,
          "[reduce gotos] Removed %zu (%zu cases) trivial switches, split %zu "
          "(packed %zu segments with %zu cases) switches, multiplexed %zu (%zu "
          "cases) switches, expanded %zu (%zu cases) switches in {%s}",
          local_stats.removed_trivial_switches,
          local_stats.removed_trivial_switch_cases,
          local_stats.splitting_transformations,
          local_stats.splitting_transformations_packed_segments,
          local_stats.splitting_transformations_switch_cases_packed,
          local_stats.multiplexing_transformations(),
          local_stats.multiplexing_switch_cases(),
          local_stats.expanded_transformations,
          local_stats.expanded_switch_cases, SHOW(method));

    TRACE(RSS, 4, "Rewrote {%s}:\n%s", SHOW(method), SHOW(cfg));

    local_stats.affected_methods++;
    std::lock_guard<std::mutex> lock_guard(stats_mutex);
    stats += local_stats;
  });

  mgr.incr_metric(METRIC_AFFECTED_METHODS, stats.affected_methods);
  mgr.incr_metric(METRIC_REMOVED_TRIVIAL_SWITCH_CASES,
                  stats.removed_trivial_switch_cases);
  mgr.incr_metric(METRIC_REMOVED_TRIVIAL_SWITCHES,
                  stats.removed_trivial_switches);

  mgr.incr_metric(METRIC_SPLITTING_TRANSFORMATIONS,
                  stats.splitting_transformations);
  mgr.incr_metric(METRIC_SPLITTING_TRANSFORMATIONS_PACKED_SEGMENTS,
                  stats.splitting_transformations_packed_segments);
  mgr.incr_metric(METRIC_SPLITTING_TRANSFORMATIONS_SWITCH_CASES_PACKED,
                  stats.splitting_transformations_switch_cases_packed);
  mgr.incr_metric(METRIC_MULTIPLEXING_TRANSFORMATIONS,
                  stats.multiplexing_transformations());
  mgr.incr_metric(METRIC_MULTIPLEXING_TRANSFORMATIONS_SWITCH_CASES,
                  stats.multiplexing_switch_cases());
  for (auto&& [M, mstats] : stats.multiplexing) {
    if (mstats.abandoned > 0) {
      mgr.incr_metric(METRIC_MULTIPLEXING_ABANDONED_PREFIX + std::to_string(M),
                      mstats.abandoned);
    }
    if (mstats.transformations > 0) {
      mgr.incr_metric(
          METRIC_MULTIPLEXING_TRANSFORMATIONS_AVERAGE_INEFFICIENCY_PREFIX +
              std::to_string(M),
          mstats.inefficiency / mstats.transformations);
    }
  }
  mgr.incr_metric(METRIC_EXPANDED_TRANSFORMATIONS,
                  stats.expanded_transformations);
  mgr.incr_metric(METRIC_EXPANDED_SWITCH_CASES, stats.expanded_switch_cases);

  TRACE(RSS, 1,
        "[reduce sparse switches] Removed %zu (%zu cases) trivial switches, "
        "split %zu (packed %zu segments with %zu cases) switches, multiplexed "
        "%zu (%zu cases) switches, expanded %zu (%zu cases) switches",
        stats.removed_trivial_switches, stats.removed_trivial_switch_cases,
        stats.splitting_transformations,
        stats.splitting_transformations_packed_segments,
        stats.splitting_transformations_switch_cases_packed,
        stats.multiplexing_transformations(), stats.multiplexing_switch_cases(),
        stats.expanded_transformations, stats.expanded_switch_cases);
  for (auto&& [M, mstats] : stats.multiplexing) {
    TRACE(RSS, 2,
          "[reduce sparse switches] M=%zu: %zu abandoned, %zu accumulated "
          "inefficiency / %zu transformed = %zu average inefficiency",
          M, mstats.abandoned, mstats.inefficiency, mstats.transformations,
          mstats.transformations == 0
              ? 0
              : mstats.inefficiency / mstats.transformations);
  }
}

size_t ReduceSparseSwitchesPass::Stats::multiplexing_transformations() const {
  return std::accumulate(
      multiplexing.begin(), multiplexing.end(), 0,
      [](size_t acc, const auto& p) { return acc + p.second.transformations; });
}

size_t ReduceSparseSwitchesPass::Stats::multiplexing_switch_cases() const {
  return std::accumulate(
      multiplexing.begin(), multiplexing.end(), 0,
      [](size_t acc, const auto& p) { return acc + p.second.switch_cases; });
}

ReduceSparseSwitchesPass::Stats::Multiplexing&
ReduceSparseSwitchesPass::Stats::Multiplexing::operator+=(
    const ReduceSparseSwitchesPass::Stats::Multiplexing& that) {
  abandoned += that.abandoned;
  transformations += that.transformations;
  switch_cases += that.switch_cases;
  inefficiency += that.inefficiency;

  return *this;
}

ReduceSparseSwitchesPass::Stats& ReduceSparseSwitchesPass::Stats::operator+=(
    const ReduceSparseSwitchesPass::Stats& that) {
  affected_methods += that.affected_methods;
  removed_trivial_switch_cases += that.removed_trivial_switch_cases;
  removed_trivial_switches += that.removed_trivial_switches;
  splitting_transformations += that.splitting_transformations;
  splitting_transformations_packed_segments +=
      that.splitting_transformations_packed_segments;
  splitting_transformations_switch_cases_packed +=
      that.splitting_transformations_switch_cases_packed;
  expanded_transformations += that.expanded_transformations;
  expanded_switch_cases += that.expanded_switch_cases;

  for (auto&& [M, mstats] : that.multiplexing) {
    multiplexing[M] += mstats;
  }
  return *this;
}

static ReduceSparseSwitchesPass s_pass;
