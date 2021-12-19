/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SourceBlocks.h"

#include <limits>
#include <optional>
#include <sstream>
#include <string>

#include "ControlFlow.h"
#include "Debug.h"
#include "DexClass.h"
#include "Dominators.h"
#include "IROpcode.h"
#include "Macros.h"
#include "S_Expression.h"
#include "ScopedMetrics.h"
#include "Show.h"
#include "Timer.h"
#include "Trace.h"
#include "Walkers.h"

namespace source_blocks {

using namespace cfg;
using namespace sparta;

namespace {

constexpr SourceBlock::Val kFailVal = SourceBlock::Val::none();
constexpr SourceBlock::Val kXVal = SourceBlock::Val::none();

struct InsertHelper {
  DexMethod* method;
  uint32_t id{0};
  std::ostringstream oss;
  bool serialize;
  bool insert_after_excs;

  struct ProfileParserState {
    s_expr root_expr;
    std::vector<s_expr> expr_stack;
    bool had_profile_failure{false};
    boost::optional<SourceBlock::Val> default_val;
    boost::optional<SourceBlock::Val> error_val;
    ProfileParserState(s_expr root_expr,
                       std::vector<s_expr> expr_stack,
                       bool had_profile_failure,
                       const boost::optional<SourceBlock::Val>& default_val,
                       const boost::optional<SourceBlock::Val>& error_val)
        : root_expr(std::move(root_expr)),
          expr_stack(std::move(expr_stack)),
          had_profile_failure(had_profile_failure),
          default_val(default_val),
          error_val(error_val) {}
  };
  std::vector<ProfileParserState> parser_state;

  InsertHelper(DexMethod* method,
               const std::vector<ProfileData>& profiles,
               bool serialize,
               bool insert_after_excs)
      : method(method),
        serialize(serialize),
        insert_after_excs(insert_after_excs) {
    for (const auto& p : profiles) {
      switch (p.index()) {
      case 0:
        // Nothing.
        parser_state.emplace_back(s_expr(), std::vector<s_expr>(), false,
                                  boost::none, boost::none);
        break;

      case 1:
        // Profile string.
        {
          const auto& pair = std::get<1>(p);
          const std::string& profile = pair.first;
          std::istringstream iss{profile};
          s_expr_istream s_expr_input(iss);
          s_expr root_expr;
          s_expr_input >> root_expr;
          always_assert_log(!s_expr_input.fail(),
                            "Failed parsing profile %s for %s: %s",
                            profile.c_str(),
                            SHOW(method),
                            s_expr_input.what().c_str());
          std::vector<s_expr> expr_stack;
          expr_stack.push_back(s_expr({root_expr}));
          parser_state.emplace_back(std::move(root_expr), std::move(expr_stack),
                                    false, boost::none, pair.second);
          break;
        }

      case 2:
        // A default Val.
        parser_state.emplace_back(s_expr(), std::vector<s_expr>(), false,
                                  std::get<2>(p), boost::none);
        break;

      default:
        not_reached();
      }
    }
  }

  static SourceBlock::Val parse_val(const std::string& val_str) {
    if (val_str == "x") {
      return kXVal;
    }
    size_t after_idx;
    float nested_val = std::stof(val_str, &after_idx); // May throw.
    always_assert_log(after_idx > 0,
                      "Could not parse first part of %s as float",
                      val_str.c_str());
    always_assert_log(after_idx + 1 < val_str.size(),
                      "Could not find separator of %s",
                      val_str.c_str());
    always_assert_log(val_str[after_idx] == ':',
                      "Did not find separating ':' in %s",
                      val_str.c_str());
    // This isn't efficient, but let's not play with low-level char* view.
    // Small-string optimization is likely enough.
    auto appear_part = val_str.substr(after_idx + 1);
    float appear100 = std::stof(appear_part, &after_idx);
    always_assert_log(after_idx == appear_part.size(),
                      "Could not parse second part of %s as float",
                      val_str.c_str());

    return SourceBlock::Val{nested_val, appear100};
  }

  void start(Block* cur) {
    if (serialize) {
      oss << "(" << id;
    }

    auto val = start_profile(cur);

    source_blocks::impl::BlockAccessor::push_source_block(
        cur, std::make_unique<SourceBlock>(method, id, val));
    ++id;

    if (insert_after_excs) {
      if (cur->cfg().get_succ_edge_of_type(cur, EdgeType::EDGE_THROW) !=
          nullptr) {
        // Nothing to do.
        return;
      }
      for (auto it = cur->begin(); it != cur->end(); ++it) {
        if (it->type != MFLOW_OPCODE) {
          continue;
        }
        if (!opcode::can_throw(it->insn->opcode())) {
          continue;
        }
        // Exclude throws (explicitly).
        if (it->insn->opcode() == OPCODE_THROW) {
          continue;
        }
        // Get to the next instruction.
        auto next_it = std::next(it);
        while (next_it != cur->end() && next_it->type != MFLOW_OPCODE) {
          ++next_it;
        }
        if (next_it == cur->end()) {
          break;
        }

        auto insert_after =
            opcode::is_move_result_any(next_it->insn->opcode()) ? next_it : it;

        // This is not really what the structure looks like, but easy to
        // parse and write. Otherwise, we would need to remember that
        // we had a nesting.

        if (serialize) {
          oss << "(" << id << ")";
        }

        auto nested_val = start_profile(cur, /*empty_inner_tail=*/true);
        it = source_blocks::impl::BlockAccessor::insert_source_block_after(
            cur, insert_after,
            std::make_unique<SourceBlock>(method, id, nested_val));

        ++id;
      }
    }
  }

  std::vector<SourceBlock::Val> start_profile(Block* cur,
                                              bool empty_inner_tail = false) {
    std::vector<SourceBlock::Val> ret;
    for (auto& p_state : parser_state) {
      ret.emplace_back(start_profile_one(cur, empty_inner_tail, p_state));
    }
    return ret;
  }

  SourceBlock::Val start_profile_one(Block* cur,
                                     bool empty_inner_tail,
                                     ProfileParserState& p_state) {
    if (p_state.had_profile_failure) {
      return kFailVal;
    }
    if (p_state.root_expr.is_nil()) {
      if (p_state.default_val) {
        return *p_state.default_val;
      }
      return kFailVal;
    }
    if (p_state.expr_stack.empty()) {
      p_state.had_profile_failure = true;
      TRACE(MMINL, 3,
            "Failed profile matching for %s: missing element for block %zu",
            SHOW(method), cur->id());
      return kFailVal;
    }
    std::string val_str;
    const s_expr& e = p_state.expr_stack.back();
    s_expr tail, inner_tail;
    if (!s_patn({s_patn({s_patn(&val_str)}, inner_tail)}, tail).match_with(e)) {
      p_state.had_profile_failure = true;
      TRACE(MMINL, 3,
            "Failed profile matching for %s: cannot match string for %s",
            SHOW(method), e.str().c_str());
      return kFailVal;
    }
    if (empty_inner_tail) {
      redex_assert(inner_tail.is_nil());
    }
    auto val = parse_val(val_str);
    TRACE(MMINL,
          5,
          "Started block with val=%f/%f. Popping %s, pushing %s + %s",
          val ? val->val : std::numeric_limits<float>::quiet_NaN(),
          val ? val->appear100 : std::numeric_limits<float>::quiet_NaN(),
          e.str().c_str(),
          tail.str().c_str(),
          inner_tail.str().c_str());
    p_state.expr_stack.pop_back();
    p_state.expr_stack.push_back(tail);
    if (!empty_inner_tail) {
      p_state.expr_stack.push_back(inner_tail);
    }
    return val;
  }

  static char get_edge_char(const Edge* e) {
    switch (e->type()) {
    case EDGE_BRANCH:
      return 'b';
    case EDGE_GOTO:
      return 'g';
    case EDGE_THROW:
      return 't';
    case EDGE_GHOST:
    case EDGE_TYPE_SIZE:
      not_reached();
    }
    not_reached(); // For GCC.
  }

  void edge(Block* cur, const Edge* e) {
    if (serialize) {
      oss << " " << get_edge_char(e);
    }
    edge_profile(cur, e);
  }

  void edge_profile(Block* cur, const Edge* e) {
    for (auto& p_state : parser_state) {
      edge_profile_one(cur, e, p_state);
    }
  }

  void edge_profile_one(Block* /*cur*/,
                        const Edge* e,
                        ProfileParserState& p_state) {
    // If running with profile, there should be at least a nil on.
    if (p_state.had_profile_failure || p_state.expr_stack.empty()) {
      return;
    }
    std::string val;
    s_expr& expr = p_state.expr_stack.back();
    s_expr tail;
    if (!s_patn({s_patn(&val)}, tail).match_with(expr)) {
      p_state.had_profile_failure = true;
      TRACE(MMINL, 3,
            "Failed profile matching for %s: cannot match string for %s",
            SHOW(method), expr.str().c_str());
      return;
    }
    std::string expected(1, get_edge_char(e));
    if (expected != val) {
      p_state.had_profile_failure = true;
      TRACE(MMINL, 3,
            "Failed profile matching for %s: edge type \"%s\" did not match "
            "expectation \"%s\"",
            SHOW(method), val.c_str(), expected.c_str());
      return;
    }
    TRACE(MMINL,
          5,
          "Matched edge %s. Popping %s, pushing %s",
          val.c_str(),
          expr.str().c_str(),
          tail.str().c_str());
    p_state.expr_stack.pop_back();
    p_state.expr_stack.push_back(tail);
  }

  void end(Block* cur) {
    if (serialize) {
      oss << ")";
    }
    end_profile(cur);
  }

  void end_profile(Block* cur) {
    for (auto& p_state : parser_state) {
      end_profile_one(cur, p_state);
    }
  }

  void end_profile_one(Block* /*cur*/, ProfileParserState& p_state) {
    if (p_state.had_profile_failure) {
      return;
    }
    if (p_state.root_expr.is_nil()) {
      return;
    }
    if (p_state.expr_stack.empty()) {
      TRACE(MMINL,
            3,
            "Failed profile matching for %s: empty stack on close",
            SHOW(method));
      p_state.had_profile_failure = true;
      return;
    }
    if (!p_state.expr_stack.back().is_nil()) {
      TRACE(MMINL,
            3,
            "Failed profile matching for %s: edge sentinel not NIL",
            SHOW(method));
      p_state.had_profile_failure = true;
      return;
    }
    TRACE(MMINL, 5, "Popping %s", p_state.expr_stack.back().str().c_str());
    p_state.expr_stack.pop_back(); // Remove sentinel nil.
  }

  bool wipe_profile_failures(ControlFlowGraph& cfg) {
    bool ret = false;
    for (size_t i = 0; i != parser_state.size(); ++i) {
      auto& p_state = parser_state[i];
      if (p_state.root_expr.is_nil()) {
        continue;
      }
      if (!p_state.had_profile_failure) {
        continue;
      }
      ret = true;
      if (traceEnabled(MMINL, 3) && serialize) {
        TRACE(MMINL, 3, "For %s, expected profile of the form %s", SHOW(method),
              oss.str().c_str());
      }
      auto val =
          p_state.error_val ? *p_state.error_val : SourceBlock::Val::none();
      for (auto* b : cfg.blocks()) {
        auto vec = gather_source_blocks(b);
        for (auto* sb : vec) {
          const_cast<SourceBlock*>(sb)->vals[i] = val;
        }
      }
    }
    return ret;
  }
};

} // namespace

InsertResult insert_source_blocks(DexMethod* method,
                                  ControlFlowGraph* cfg,
                                  const std::vector<ProfileData>& profiles,
                                  bool serialize,
                                  bool insert_after_excs) {
  InsertHelper helper(method, profiles, serialize, insert_after_excs);

  impl::visit_in_order(
      cfg, [&](Block* cur) { helper.start(cur); },
      [&](Block* cur, const Edge* e) { helper.edge(cur, e); },
      [&](Block* cur) { helper.end(cur); });

  bool had_failures = helper.wipe_profile_failures(*cfg);

  return {helper.id, helper.oss.str(), !had_failures};
}

bool has_source_block_positive_val(const SourceBlock* sb) {
  bool any_positive_val = false;
  if (sb != nullptr) {
    sb->foreach_val_early([&any_positive_val](const auto& val) {
      any_positive_val = val && val->val > 0.0f;
      return any_positive_val;
    });
  }
  return any_positive_val;
}

namespace {

struct SourceBlocksStats {
  size_t total_blocks{0};
  size_t source_blocks_present{0};
  size_t source_blocks_total{0};
  size_t methods_with_sbs{0};
  size_t flow_violation_idom{0};
  size_t flow_violation_direct_predecessors{0};
  size_t flow_violation_cold_direct_predecessors{0};
  size_t methods_with_cold_direct_predecessor_violations{0};
  size_t methods_with_idom_violations{0};
  size_t methods_with_direct_predecessor_violations{0};
  size_t methods_with_code{0};

  SourceBlocksStats& operator+=(const SourceBlocksStats& that) {
    total_blocks += that.total_blocks;
    source_blocks_present += that.source_blocks_present;
    source_blocks_total += that.source_blocks_total;
    methods_with_sbs += that.methods_with_sbs;
    flow_violation_idom += that.flow_violation_idom;
    flow_violation_direct_predecessors +=
        that.flow_violation_direct_predecessors;
    flow_violation_cold_direct_predecessors +=
        that.flow_violation_cold_direct_predecessors;
    methods_with_cold_direct_predecessor_violations +=
        that.methods_with_cold_direct_predecessor_violations;
    methods_with_idom_violations += that.methods_with_idom_violations;
    methods_with_direct_predecessor_violations +=
        that.methods_with_direct_predecessor_violations;
    methods_with_code += that.methods_with_code;
    return *this;
  }
};

bool is_source_block_hot(SourceBlock* sb) {
  bool is_hot = false;
  if (sb != nullptr) {
    sb->foreach_val_early([&is_hot](const auto& val) {
      is_hot = val && val->val > 0.0f;
      return is_hot;
    });
  }
  return is_hot;
}

} // namespace

void track_source_block_coverage(ScopedMetrics& sm,
                                 const DexStoresVector& stores) {
  Timer opt_timer("Calculate SourceBlock Coverage");
  auto stats = walk::parallel::methods<SourceBlocksStats>(
      build_class_scope(stores), [](DexMethod* m) -> SourceBlocksStats {
        SourceBlocksStats ret;
        auto code = m->get_code();
        if (!code) {
          return ret;
        }
        ret.methods_with_code++;
        code->build_cfg(/* editable */ true);
        auto& cfg = code->cfg();
        auto dominators =
            dominators::SimpleFastDominators<cfg::GraphInterface>(cfg);

        bool seen_dir_cold_dir_pred = false;
        bool seen_idom_viol = false;
        bool seen_direct_pred_viol = false;
        bool seen_sb = false;
        for (auto block : cfg.blocks()) {
          ret.total_blocks++;
          if (source_blocks::has_source_blocks(block)) {
            ret.source_blocks_present++;
            seen_sb = true;
            source_blocks::foreach_source_block(
                block,
                [&](auto* sb ATTRIBUTE_UNUSED) { ret.source_blocks_total++; });
          }
          if (block != cfg.entry_block()) {
            auto immediate_dominator = dominators.get_idom(block);
            if (!immediate_dominator) {
              continue;
            }
            auto* first_sb_immediate_dominator =
                source_blocks::get_first_source_block(immediate_dominator);
            auto* first_sb_current_b =
                source_blocks::get_first_source_block(block);

            bool is_idom_hot =
                is_source_block_hot(first_sb_immediate_dominator);
            bool is_curr_block_hot = is_source_block_hot(first_sb_current_b);

            if (!is_idom_hot && is_curr_block_hot) {
              ret.flow_violation_idom++;
              seen_idom_viol = true;
            }

            // If current block is hot, one of its predecessors must also be
            // hot. If all predecessors of a block are cold, the block must also
            // be cold
            if (is_curr_block_hot) {
              auto found_hot_pred = [&]() {
                for (auto predecessor : block->preds()) {
                  auto* first_sb_pred =
                      source_blocks::get_first_source_block(predecessor->src());
                  if (is_source_block_hot(first_sb_pred)) {
                    return true;
                  }
                }
                return false;
              }();
              if (!found_hot_pred) {
                ret.flow_violation_direct_predecessors++;
                seen_direct_pred_viol = true;
              }

              bool all_predecessors_cold = [&]() {
                for (auto predecessor : block->preds()) {
                  auto* first_sb_pred =
                      source_blocks::get_first_source_block(predecessor->src());
                  if (is_source_block_hot(first_sb_pred)) {
                    return false;
                  }
                }
                return true;
              }();
              if (all_predecessors_cold) {
                ret.flow_violation_cold_direct_predecessors++;
                seen_dir_cold_dir_pred = true;
              }
            }
          }
        }
        if (seen_dir_cold_dir_pred) {
          ret.methods_with_cold_direct_predecessor_violations++;
        }
        if (seen_idom_viol) {
          ret.methods_with_idom_violations++;
        }
        if (seen_direct_pred_viol) {
          ret.methods_with_direct_predecessor_violations++;
        }

        if (seen_sb) {
          ret.methods_with_sbs++;
        }

        code->clear_cfg();
        return ret;
      });

  sm.set_metric("~assessment~methods~with~code", stats.methods_with_code);
  sm.set_metric("~blocks~count", stats.total_blocks);
  sm.set_metric("~blocks~with~source~blocks", stats.source_blocks_present);
  sm.set_metric("~assessment~source~blocks~total", stats.source_blocks_total);
  sm.set_metric("~assessment~methods~with~sbs", stats.methods_with_sbs);
  sm.set_metric("~flow~violation~idom", stats.flow_violation_idom);
  sm.set_metric("~flow~violation~methods~idom",
                stats.methods_with_idom_violations);
  sm.set_metric("~flow~violation~direct~predecessors",
                stats.flow_violation_direct_predecessors);
  sm.set_metric("~flow~violation~methods~direct~predecessors",
                stats.methods_with_direct_predecessor_violations);
  sm.set_metric("~flow~violation~cold~direct~predecessors",
                stats.flow_violation_cold_direct_predecessors);
  sm.set_metric("~flow~violation~methods~direct~cold~predecessors",
                stats.methods_with_cold_direct_predecessor_violations);
}

} // namespace source_blocks
