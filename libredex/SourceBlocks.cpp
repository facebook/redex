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

size_t count_blocks(
    Block*, const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  return 1;
}
size_t count_block_has_sbs(
    Block* b, const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  return source_blocks::has_source_blocks(b) ? 1 : 0;
}
size_t count_all_sbs(
    Block* b, const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  size_t ret{0};
  source_blocks::foreach_source_block(
      b, [&](auto* sb ATTRIBUTE_UNUSED) { ++ret; });
  return ret;
}

// TODO: Per-interaction stats.

size_t hot_immediate_dom_not_hot(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>& dominators) {
  auto* first_sb_current_b = source_blocks::get_first_source_block(block);
  if (!is_source_block_hot(first_sb_current_b)) {
    return 0;
  }

  auto immediate_dominator = dominators.get_idom(block);
  if (!immediate_dominator) {
    return 0;
  }
  auto* first_sb_immediate_dominator =
      source_blocks::get_first_source_block(immediate_dominator);
  bool is_idom_hot = is_source_block_hot(first_sb_immediate_dominator);
  return is_idom_hot ? 0 : 1;
}

// TODO: This needs to be adapted to sum up the predecessors.
size_t hot_no_hot_pred(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  auto* first_sb_current_b = source_blocks::get_first_source_block(block);
  if (!is_source_block_hot(first_sb_current_b)) {
    return 0;
  }

  for (auto predecessor : block->preds()) {
    auto* first_sb_pred =
        source_blocks::get_first_source_block(predecessor->src());
    if (is_source_block_hot(first_sb_pred)) {
      return 0;
    }
  }
  return 1;
}

// TODO: Isn't that the same as before, just this time correct w/ counting?
size_t hot_all_pred_cold(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  auto* first_sb_current_b = source_blocks::get_first_source_block(block);
  if (!is_source_block_hot(first_sb_current_b)) {
    return 0;
  }

  for (auto predecessor : block->preds()) {
    auto* first_sb_pred =
        source_blocks::get_first_source_block(predecessor->src());
    if (is_source_block_hot(first_sb_pred)) {
      return 0;
    }
  }
  return 1;
}

// Ugly but necessary for constexpr below.
using CounterFnPtr = size_t (*)(
    Block*, const dominators::SimpleFastDominators<cfg::GraphInterface>&);

constexpr std::array<std::pair<std::string_view, CounterFnPtr>, 3> gCounters = {
    {
        {"~blocks~count", &count_blocks},
        {"~blocks~with~source~blocks", &count_block_has_sbs},
        {"~assessment~source~blocks~total", &count_all_sbs},
    }};

constexpr std::array<std::pair<std::string_view, CounterFnPtr>, 3>
    gCountersNonEntry = {{
        {"~flow~violation~idom", &hot_immediate_dom_not_hot},
        {"~flow~violation~direct~predecessors", &hot_no_hot_pred},
        {"~flow~violation~cold~direct~predecessors", &hot_all_pred_cold},
    }};

struct SourceBlocksStats {
  size_t methods_with_code{0};
  size_t methods_with_sbs{0};

  std::array<size_t, gCounters.size()> global{};

  std::array<size_t, gCountersNonEntry.size()> non_entry{};
  std::array<size_t, gCountersNonEntry.size()> non_entry_methods{};
  std::array<std::pair<size_t, size_t>, gCountersNonEntry.size()>
      non_entry_min_max{};
  std::array<std::pair<const DexMethod*, const DexMethod*>,
             gCountersNonEntry.size()>
      non_entry_min_max_methods{};

  SourceBlocksStats& operator+=(const SourceBlocksStats& that) {
    methods_with_code += that.methods_with_code;
    methods_with_sbs += that.methods_with_sbs;

    for (size_t i = 0; i != global.size(); ++i) {
      global[i] += that.global[i];
    }

    for (size_t i = 0; i != non_entry.size(); ++i) {
      non_entry[i] += that.non_entry[i];
    }
    for (size_t i = 0; i != non_entry_methods.size(); ++i) {
      non_entry_methods[i] += that.non_entry_methods[i];
    }
    for (size_t i = 0; i != non_entry_min_max.size(); ++i) {
      non_entry_min_max[i].first =
          std::min(non_entry_min_max[i].first, that.non_entry_min_max[i].first);
      non_entry_min_max[i].second = std::max(non_entry_min_max[i].second,
                                             that.non_entry_min_max[i].second);
    }

    for (size_t i = 0; i != non_entry_min_max_methods.size(); ++i) {
      auto set_min_max = [](auto& lhs, auto& rhs, auto fn) {
        if (rhs == nullptr) {
          return;
        }
        if (lhs == nullptr) {
          lhs = rhs;
          return;
        }
        auto lhs_count = lhs->get_code()->count_opcodes();
        auto rhs_count = rhs->get_code()->count_opcodes();
        auto op = fn(lhs_count, rhs_count);
        if (op == rhs_count &&
            (op != lhs_count || compare_dexmethods(rhs, lhs))) {
          lhs = rhs;
        }
      };

      set_min_max(non_entry_min_max_methods[i].first,
                  that.non_entry_min_max_methods[i].first,
                  [](auto lhs, auto rhs) { return std::min(lhs, rhs); });
      set_min_max(non_entry_min_max_methods[i].second,
                  that.non_entry_min_max_methods[i].second,
                  [](auto lhs, auto rhs) { return std::max(lhs, rhs); });
    }

    return *this;
  }

  void fill_derived(const DexMethod* m) {
    methods_with_code = 1;

    static_assert(gCounters[1].first == "~blocks~with~source~blocks");
    methods_with_sbs = global[1] > 0 ? 1 : 0;

    for (size_t i = 0; i != non_entry.size(); ++i) {
      non_entry_methods[i] = non_entry[i] > 0 ? 1 : 0;
      non_entry_min_max[i].first = non_entry[i];
      non_entry_min_max[i].second = non_entry[i];

      if (non_entry[i] != 0) {
        non_entry_min_max_methods[i].first = m;
        non_entry_min_max_methods[i].second = m;
      }
    }
  }
};

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
        code->build_cfg(/* editable */ true);
        auto& cfg = code->cfg();
        auto dominators =
            dominators::SimpleFastDominators<cfg::GraphInterface>(cfg);

        bool seen_dir_cold_dir_pred = false;
        bool seen_idom_viol = false;
        bool seen_direct_pred_viol = false;
        bool seen_sb = false;
        for (auto block : cfg.blocks()) {
          for (size_t i = 0; i != gCounters.size(); ++i) {
            ret.global[i] += (*gCounters[i].second)(block, dominators);
          }
          if (block != cfg.entry_block()) {
            for (size_t i = 0; i != gCountersNonEntry.size(); ++i) {
              ret.non_entry[i] +=
                  (*gCountersNonEntry[i].second)(block, dominators);
            }
          }
        }

        ret.fill_derived(m);

        code->clear_cfg();
        return ret;
      });

  sm.set_metric("~assessment~methods~with~code", stats.methods_with_code);
  sm.set_metric("~assessment~methods~with~sbs", stats.methods_with_sbs);

  for (size_t i = 0; i != gCounters.size(); ++i) {
    sm.set_metric(gCounters[i].first, stats.global[i]);
  }

  for (size_t i = 0; i != gCountersNonEntry.size(); ++i) {
    sm.set_metric(gCountersNonEntry[i].first, stats.non_entry[i]);

    auto scope = sm.scope(std::string(gCountersNonEntry[i].first));
    sm.set_metric("methods", stats.non_entry_methods[i]);
    sm.set_metric("min", stats.non_entry_min_max[i].first);
    sm.set_metric("max", stats.non_entry_min_max[i].second);

    auto min_max = [&](const DexMethod* m, const char* name) {
      if (m != nullptr) {
        auto min_max_scope = sm.scope(name);
        sm.set_metric(show_deobfuscated(m), m->get_code()->count_opcodes());
      }
    };
    min_max(stats.non_entry_min_max_methods[i].first, "min_method");
    min_max(stats.non_entry_min_max_methods[i].second, "max_method");
  }
}

} // namespace source_blocks
