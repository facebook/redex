/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SourceBlocks.h"

#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

#include "ControlFlow.h"
#include "Debug.h"
#include "DexClass.h"
#include "Dominators.h"
#include "IRList.h"
#include "IROpcode.h"
#include "Macros.h"
#include "S_Expression.h"
#include "ScopedCFG.h"
#include "ScopedMetrics.h"
#include "Show.h"
#include "ShowCFG.h"
#include "SourceBlockConsistencyCheck.h"
#include "Timer.h"
#include "Trace.h"
#include "Walkers.h"

namespace source_blocks {

using namespace cfg;
using namespace sparta;

namespace {

constexpr SourceBlock::Val kFailVal = SourceBlock::Val::none();
constexpr SourceBlock::Val kXVal = SourceBlock::Val::none();

static SourceBlockConsistencyCheck s_sbcc;

struct InsertHelper {
  std::ostringstream oss;
  const DexString* method;
  uint32_t id{0};
  bool serialize;
  bool insert_after_excs;

  struct ProfileParserState {
    std::vector<s_expr> expr_stack;
    bool had_profile_failure{false};
    bool root_expr_is_nil;
    const SourceBlock::Val* default_val;
    const SourceBlock::Val* error_val;
    ProfileParserState(std::vector<s_expr> expr_stack,
                       bool root_expr_is_nil,
                       const SourceBlock::Val* default_val,
                       const SourceBlock::Val* error_val)
        : expr_stack(std::move(expr_stack)),
          root_expr_is_nil(root_expr_is_nil),
          default_val(default_val),
          error_val(error_val) {}
  };
  std::vector<ProfileParserState> parser_state;
  struct MatcherState {
    const std::string* val_str_ptr;
    s_expr tail;
    s_expr inner_tail;
    s_patn val_str_inner_tail_tail_pattern;
    s_patn val_str_tail_pattern;
    MatcherState()
        : val_str_inner_tail_tail_pattern(
              {s_patn({s_patn(&val_str_ptr)}, inner_tail)}, tail),
          val_str_tail_pattern({s_patn(&val_str_ptr)}, tail) {}
  };
  MatcherState matcher_state;

  InsertHelper(const DexString* method,
               const std::vector<ProfileData>& profiles,
               bool serialize,
               bool insert_after_excs)
      : method(method),
        serialize(serialize),
        insert_after_excs(insert_after_excs) {
    parser_state.reserve(profiles.size());
    for (const auto& p : profiles) {
      switch (p.index()) {
      case 0:
        // Nothing.
        parser_state.emplace_back(std::vector<s_expr>(),
                                  /* root_expr_is_nil */ true,
                                  /* default_val */ nullptr,
                                  /* error_val */ nullptr);
        break;

      case 1:
        // Profile string.
        {
          const auto& [profile, error_val_opt] = std::get<1>(p);
          std::istringstream iss{profile};
          s_expr_istream s_expr_input(iss);
          s_expr root_expr;
          s_expr_input >> root_expr;
          always_assert_log(!s_expr_input.fail(),
                            "Failed parsing profile %s for %s: %s",
                            profile.c_str(),
                            SHOW(method),
                            s_expr_input.what().c_str());
          bool root_expr_is_nil = root_expr.is_nil();
          std::vector<s_expr> expr_stack{s_expr({std::move(root_expr)})};
          auto* error_val = error_val_opt ? &*error_val_opt : nullptr;
          parser_state.emplace_back(std::move(expr_stack), root_expr_is_nil,
                                    /* default_val */ nullptr, error_val);
          break;
        }

      case 2: {
        // A default Val.
        auto* default_val = &std::get<2>(p);
        parser_state.emplace_back(std::vector<s_expr>(),
                                  /* root_expr_is_nil */ true, default_val,
                                  /* error_val */ nullptr);
        break;
      }

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
    ret.reserve(parser_state.size());
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
    if (p_state.root_expr_is_nil) {
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
    s_expr& top_expr = p_state.expr_stack.back();
    if (!matcher_state.val_str_inner_tail_tail_pattern.match_with(top_expr)) {
      p_state.had_profile_failure = true;
      TRACE(MMINL, 3,
            "Failed profile matching for %s: cannot match string for %s",
            SHOW(method), top_expr.str().c_str());
      return kFailVal;
    }
    if (empty_inner_tail) {
      redex_assert(matcher_state.inner_tail.is_nil());
    }
    auto val = parse_val(*matcher_state.val_str_ptr);
    TRACE(MMINL,
          5,
          "Started block with val=%f/%f. Popping %s, pushing %s + %s",
          val ? val->val : std::numeric_limits<float>::quiet_NaN(),
          val ? val->appear100 : std::numeric_limits<float>::quiet_NaN(),
          top_expr.str().c_str(),
          matcher_state.tail.str().c_str(),
          matcher_state.inner_tail.str().c_str());
    top_expr = std::move(matcher_state.tail);
    if (!empty_inner_tail) {
      p_state.expr_stack.push_back(std::move(matcher_state.inner_tail));
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
    s_expr& top_expr = p_state.expr_stack.back();
    if (!matcher_state.val_str_tail_pattern.match_with(top_expr)) {
      p_state.had_profile_failure = true;
      TRACE(MMINL, 3,
            "Failed profile matching for %s: cannot match string for %s",
            SHOW(method), top_expr.str().c_str());
      return;
    }
    char edge_char = get_edge_char(e);
    std::string_view expected(&edge_char, 1);
    if (expected != *matcher_state.val_str_ptr) {
      p_state.had_profile_failure = true;
      TRACE(MMINL, 3,
            "Failed profile matching for %s: edge type \"%s\" did not match "
            "expectation \"%s\"",
            SHOW(method), matcher_state.val_str_ptr->c_str(),
            str_copy(expected).c_str());
      return;
    }
    TRACE(MMINL,
          5,
          "Matched edge %s. Popping %s, pushing %s",
          matcher_state.val_str_ptr->c_str(),
          top_expr.str().c_str(),
          matcher_state.tail.str().c_str());
    top_expr = std::move(matcher_state.tail);
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
    if (p_state.root_expr_is_nil) {
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
    s_expr& top_expr = p_state.expr_stack.back();
    if (!top_expr.is_nil()) {
      TRACE(MMINL,
            3,
            "Failed profile matching for %s: edge sentinel not NIL",
            SHOW(method));
      p_state.had_profile_failure = true;
      return;
    }
    TRACE(MMINL, 5, "Popping %s", top_expr.str().c_str());
    p_state.expr_stack.pop_back(); // Remove sentinel nil.
  }

  bool wipe_profile_failures(ControlFlowGraph& cfg) {
    bool ret = false;
    for (size_t i = 0; i != parser_state.size(); ++i) {
      auto& p_state = parser_state[i];
      if (p_state.root_expr_is_nil) {
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

SourceBlockConsistencyCheck& get_sbcc() { return s_sbcc; }

InsertResult insert_source_blocks(DexMethod* method,
                                  ControlFlowGraph* cfg,
                                  const std::vector<ProfileData>& profiles,
                                  bool serialize,
                                  bool insert_after_excs) {
  InsertHelper helper(&method->get_deobfuscated_name(), profiles, serialize,
                      insert_after_excs);

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

IRList::iterator find_first_block_insert_point(cfg::Block* b) {
  // Do not put source blocks before a (pseudo) move result at the head of a
  // block.
  auto it = b->begin();
  if (it == b->end()) {
    return it;
  }
  if (it->type == MFLOW_OPCODE) {
    auto op = it->insn->opcode();
    if (opcode::is_a_move_result(op) || opcode::is_a_move_result_pseudo(op) ||
        opcode::is_move_exception(op)) {
      ++it;
    }
  }
  return it;
}

namespace {

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
  if (!has_source_block_positive_val(first_sb_current_b)) {
    return 0;
  }

  auto immediate_dominator = dominators.get_idom(block);
  if (!immediate_dominator) {
    return 0;
  }
  auto* first_sb_immediate_dominator =
      source_blocks::get_first_source_block(immediate_dominator);
  bool is_idom_hot =
      has_source_block_positive_val(first_sb_immediate_dominator);
  return is_idom_hot ? 0 : 1;
}

// TODO: This needs to be adapted to sum up the predecessors.
size_t hot_no_hot_pred(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  auto* first_sb_current_b = source_blocks::get_first_source_block(block);
  if (!has_source_block_positive_val(first_sb_current_b)) {
    return 0;
  }

  for (auto predecessor : block->preds()) {
    auto* first_sb_pred =
        source_blocks::get_first_source_block(predecessor->src());
    if (has_source_block_positive_val(first_sb_pred)) {
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
  if (!has_source_block_positive_val(first_sb_current_b)) {
    return 0;
  }

  for (auto predecessor : block->preds()) {
    auto* first_sb_pred =
        source_blocks::get_first_source_block(predecessor->src());
    if (has_source_block_positive_val(first_sb_pred)) {
      return 0;
    }
  }
  return 1;
}

template <typename Fn>
size_t chain_hot_violations_tmpl(Block* block, const Fn& fn) {
  size_t sum{0};
  for (auto& mie : *block) {
    if (mie.type != MFLOW_SOURCE_BLOCK) {
      continue;
    }

    for (auto* sb = mie.src_block.get(); sb->next != nullptr;
         sb = sb->next.get()) {
      // Check that each interaction has at least as high a hit value as the
      // next SourceBlock.
      auto* next = sb->next.get();
      size_t local_sum{0};
      for (size_t i = 0; i != sb->vals_size; ++i) {
        auto sb_val = sb->get_val(i);
        auto next_val = next->get_val(i);
        if (sb_val) {
          if (next_val && *sb_val < *next_val) {
            ++local_sum;
          }
        } else if (next_val) {
          ++local_sum;
        }
      }
      sum += fn(local_sum);
    }
  }

  return sum;
}

size_t chain_hot_violations(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  return chain_hot_violations_tmpl(block, [](auto val) { return val; });
}

size_t chain_hot_one_violations(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  return chain_hot_violations_tmpl(block,
                                   [](auto val) { return val > 0 ? 1 : 0; });
}

struct ChainAndDomState {
  const SourceBlock* last{nullptr};
  cfg::Block* dom_block{nullptr};
  size_t violations{0};
};

void chain_and_dom_update(
    cfg::Block* block,
    const SourceBlock* sb,
    bool first_in_block,
    ChainAndDomState& state,
    const dominators::SimpleFastDominators<cfg::GraphInterface>& dom) {
  if (first_in_block) {
    state.last = nullptr;
    for (auto* b = dom.get_idom(block); state.last == nullptr && b != nullptr;
         b = dom.get_idom(b)) {
      state.last = get_last_source_block(b);
      if (b == b->cfg().entry_block()) {
        state.dom_block = b;
        break;
      }
    }
  } else {
    state.dom_block = nullptr;
  }

  if (state.last != nullptr) {
    for (size_t i = 0; i != sb->vals_size; ++i) {
      auto last_val = state.last->get_val(i);
      auto sb_val = sb->get_val(i);
      if (last_val) {
        if (sb_val && *last_val < *sb_val) {
          state.violations++;
          break;
        }
      } else if (sb_val && *sb_val > 0) {
        // Treat 'x' and '0' the same for violations for now.
        state.violations++;
        break;
      }
    }
  }

  state.last = sb;
}

size_t chain_and_dom_violations(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>& dom) {
  ChainAndDomState state{};

  bool first = true;
  foreach_source_block(block, [block, &state, &first, &dom](const auto* sb) {
    chain_and_dom_update(block, sb, first, state, dom);
    first = false;
  });

  return state.violations;
}

// Ugly but necessary for constexpr below.
using CounterFnPtr = size_t (*)(
    Block*, const dominators::SimpleFastDominators<cfg::GraphInterface>&);

constexpr std::array<std::pair<std::string_view, CounterFnPtr>, 6> gCounters = {
    {
        {"~blocks~count", &count_blocks},
        {"~blocks~with~source~blocks", &count_block_has_sbs},
        {"~assessment~source~blocks~total", &count_all_sbs},
        {"~flow~violation~in~chain", &chain_hot_violations},
        {"~flow~violation~in~chain~one", &chain_hot_one_violations},
        {"~flow~violation~chain~and~dom", &chain_and_dom_violations},
    }};

constexpr std::array<std::pair<std::string_view, CounterFnPtr>, 3>
    gCountersNonEntry = {{
        {"~flow~violation~idom", &hot_immediate_dom_not_hot},
        {"~flow~violation~direct~predecessors", &hot_no_hot_pred},
        {"~flow~violation~cold~direct~predecessors", &hot_all_pred_cold},
    }};

struct SourceBlocksStats {
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

  size_t consistency_check_violations =
      s_sbcc.is_initialized() ? s_sbcc.run(build_class_scope(stores)) : 0;

  sm.set_metric("~consistency~check~violations", consistency_check_violations);

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

struct ViolationsHelper::ViolationsHelperImpl {
  std::unordered_map<DexMethod*, size_t> violations_start;
  std::vector<std::string> print;

  using Violation = ViolationsHelper::Violation;
  const Violation v;

  ViolationsHelperImpl(Violation v,
                       const Scope& scope,
                       std::vector<std::string> to_vis)
      : print(std::move(to_vis)), v(v) {
    {
      std::mutex lock;
      walk::parallel::methods(scope, [this, &lock, v](DexMethod* m) {
        if (m->get_code() == nullptr) {
          return;
        }
        cfg::ScopedCFG cfg(m->get_code());
        auto val = compute(v, *cfg);
        {
          std::unique_lock<std::mutex> ulock{lock};
          violations_start[m] = val;
        }
      });
    }

    print_all();
  }

  static size_t compute(Violation v, cfg::ControlFlowGraph& cfg) {
    switch (v) {
    case Violation::kHotImmediateDomNotHot:
      return hot_immediate_dom_not_hot_cfg(cfg);
    case Violation::kChainAndDom:
      return chain_and_dom_violations_cfg(cfg);
    }
    not_reached();
  }

  ~ViolationsHelperImpl() {
    std::atomic<size_t> change_sum{0};

    {
      std::mutex lock;

      struct MethodDelta {
        DexMethod* method;
        size_t violations_delta;
        size_t method_size;

        MethodDelta(DexMethod* p1, size_t p2, size_t p3)
            : method(p1), violations_delta(p2), method_size(p3) {}
      };

      std::vector<MethodDelta> top_changes;
      constexpr size_t kTopChanges = 10;

      workqueue_run<std::pair<DexMethod*, size_t>>(
          [&](const std::pair<DexMethod*, size_t>& p) {
            auto* m = p.first;
            if (m->get_code() == nullptr) {
              return;
            }
            cfg::ScopedCFG cfg(m->get_code());
            auto val = compute(v, *cfg);
            if (val <= p.second) {
              return;
            }
            change_sum.fetch_add(val - p.second);

            auto m_delta = val - p.second;
            size_t s = m->get_code()->sum_opcode_sizes();
            std::unique_lock<std::mutex> ulock{lock};
            if (top_changes.size() < kTopChanges) {
              top_changes.emplace_back(m, m_delta, s);
              return;
            }
            MethodDelta m_t{m, m_delta, s};
            auto cmp = [](const auto& t1, const auto& t2) {
              if (t1.violations_delta > t2.violations_delta) {
                return true;
              }
              if (t1.violations_delta < t2.violations_delta) {
                return false;
              }

              if (t1.method_size < t2.method_size) {
                return true;
              }
              if (t1.method_size > t2.method_size) {
                return false;
              }

              return compare_dexmethods(t1.method, t2.method);
            };

            if (cmp(m_t, top_changes.back())) {
              top_changes.back() = m_t;
              std::sort(top_changes.begin(), top_changes.end(), cmp);
            }
          },
          violations_start);

      for (auto& t : top_changes) {
        TRACE(MMINL, 0, "%s (size %zu): +%zu", SHOW(t.method), t.method_size,
              t.violations_delta);
      }
    }

    print_all();

    TRACE(MMINL, 0, "Introduced %zu violations.", change_sum.load());
  }

  static size_t hot_immediate_dom_not_hot_cfg(cfg::ControlFlowGraph& cfg) {
    size_t sum{0};
    dominators::SimpleFastDominators<cfg::GraphInterface> dom{cfg};
    for (auto* b : cfg.blocks()) {
      sum += hot_immediate_dom_not_hot(b, dom);
    }
    return sum;
  }

  static size_t chain_and_dom_violations_cfg(cfg::ControlFlowGraph& cfg) {
    size_t sum{0};
    dominators::SimpleFastDominators<cfg::GraphInterface> dom{cfg};
    for (auto* b : cfg.blocks()) {
      sum += chain_and_dom_violations(b, dom);
    }
    return sum;
  }

  void print_all() const {
    for (const auto& m_str : print) {
      auto* m = DexMethod::get_method(m_str);
      if (m != nullptr) {
        redex_assert(m != nullptr && m->is_def());
        auto* m_def = m->as_def();
        print_cfg_with_violations(v, m_def);
      }
    }
  }

  template <typename SpecialT>
  static void print_cfg_with_violations(DexMethod* m) {
    cfg::ScopedCFG cfg(m->get_code());
    SpecialT special{*cfg};
    TRACE(MMINL, 0, "=== %s ===\n%s\n", SHOW(m),
          show<SpecialT>(*cfg, special).c_str());
  }

  static void print_cfg_with_violations(Violation v, DexMethod* m) {
    switch (v) {
    case Violation::kHotImmediateDomNotHot: {
      struct HotImmediateSpecial {
        cfg::Block* cur{nullptr};
        dominators::SimpleFastDominators<cfg::GraphInterface> dom;

        explicit HotImmediateSpecial(cfg::ControlFlowGraph& cfg)
            : dom(dominators::SimpleFastDominators<cfg::GraphInterface>(cfg)) {}

        void mie_before(std::ostream&, const MethodItemEntry&) {}
        void mie_after(std::ostream& os, const MethodItemEntry& mie) {
          if (mie.type != MFLOW_SOURCE_BLOCK) {
            return;
          }

          auto immediate_dominator = dom.get_idom(cur);
          if (!immediate_dominator) {
            os << " NO DOMINATOR\n";
            return;
          }

          if (!source_blocks::has_source_block_positive_val(
                  mie.src_block.get())) {
            os << " NOT HOT\n";
            return;
          }

          auto* first_sb_immediate_dominator =
              source_blocks::get_first_source_block(immediate_dominator);
          if (!first_sb_immediate_dominator) {
            os << " NO DOMINATOR SOURCE BLOCK B" << immediate_dominator->id()
               << "\n";
            return;
          }

          bool is_idom_hot = source_blocks::has_source_block_positive_val(
              first_sb_immediate_dominator);
          if (is_idom_hot) {
            os << " DOMINATOR HOT\n";
            return;
          }

          os << " !!! B" << immediate_dominator->id() << ": ";
          auto sb = first_sb_immediate_dominator;
          os << " \"" << show(sb->src) << "\"@" << sb->id;
          for (size_t i = 0; i < sb->vals_size; i++) {
            auto& val = sb->vals[i];
            os << " ";
            if (val) {
              os << val->val << "/" << val->appear100;
            } else {
              os << "N/A";
            }
          }
          os << "\n";
        }

        void start_block(std::ostream&, cfg::Block* b) { cur = b; }
        void end_block(std::ostream&, cfg::Block*) { cur = nullptr; }
      };
      print_cfg_with_violations<HotImmediateSpecial>(m);
      return;
    }
    case Violation::kChainAndDom: {
      struct ChainAndDom {
        cfg::Block* cur{nullptr};
        ChainAndDomState state{};
        bool first_in_block{false};

        dominators::SimpleFastDominators<cfg::GraphInterface> dom;

        explicit ChainAndDom(cfg::ControlFlowGraph& cfg)
            : dom(dominators::SimpleFastDominators<cfg::GraphInterface>(cfg)) {}

        void mie_before(std::ostream&, const MethodItemEntry&) {}
        void mie_after(std::ostream& os, const MethodItemEntry& mie) {
          if (mie.type != MFLOW_SOURCE_BLOCK) {
            return;
          }

          size_t old_count = state.violations;

          auto* sb = mie.src_block.get();

          chain_and_dom_update(cur, sb, first_in_block, state, dom);
          first_in_block = false;

          const bool head_error = state.violations > old_count;
          const auto* dom_block = state.dom_block;

          for (auto* cur_sb = sb->next.get(); cur_sb != nullptr;
               cur_sb = cur_sb->next.get()) {
            chain_and_dom_update(cur, cur_sb, false, state, dom);
          }

          if (state.violations > old_count) {
            os << " !!!";
            if (head_error) {
              os << " HEAD";
              if (dom_block != nullptr) {
                os << " (B" << dom_block->id() << ")";
              }
            }
            auto other = state.violations - old_count - (head_error ? 1 : 0);
            if (other > 0) {
              os << " IN_CHAIN " << other;
            }
            os << "\n";
          }
        }

        void start_block(std::ostream&, cfg::Block* b) {
          cur = b;
          first_in_block = true;
        }
        void end_block(std::ostream&, cfg::Block*) { cur = nullptr; }
      };
      print_cfg_with_violations<ChainAndDom>(m);
      return;
    }
    }
    not_reached();
  }
};

ViolationsHelper::ViolationsHelper(Violation v,
                                   const Scope& scope,
                                   std::vector<std::string> to_vis)
    : impl(std::make_unique<ViolationsHelperImpl>(
          v, scope, std::move(to_vis))) {}
ViolationsHelper::~ViolationsHelper() {}

} // namespace source_blocks
