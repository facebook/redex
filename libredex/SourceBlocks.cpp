/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SourceBlocks.h"

#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include <sparta/S_Expression.h>

#include "ControlFlow.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexStructure.h"
#include "Dominators.h"
#include "IRList.h"
#include "IROpcode.h"
#include "Macros.h"
#include "RedexContext.h"
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

const SourceBlock::Val global_default_val = SourceBlock::Val(1, 1);

// Helper function to insert source blocks after throwing instructions
template <typename ProfileFn>
void insert_after_exceptions_impl(Block* cur,
                                  const DexString* method,
                                  uint32_t& id,
                                  bool serialize,
                                  std::ostringstream& oss,
                                  ProfileFn profile_fn) {
  if (cur->cfg().get_succ_edge_of_type(cur, EdgeType::EDGE_THROW) != nullptr) {
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

    auto nested_val = profile_fn();
    it = source_blocks::impl::BlockAccessor::insert_source_block_after(
        cur, insert_after,
        std::make_unique<SourceBlock>(method, id, nested_val));

    ++id;
  }
}

bool is_less_than_for_any_value(
    const SourceBlock* lhs,
    const SourceBlock* rhs,
    uint32_t max_interaction = std::numeric_limits<uint32_t>::max()) {
  auto limit =
      std::min(std::min(lhs->vals_size, rhs->vals_size), max_interaction);
  for (size_t i = 0; i != limit; ++i) {
    if (lhs->get_val(i).get_value_or(0) < rhs->get_val(i).get_value_or(0)) {
      return true;
    }
  }
  return false;
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

bool is_ghost_block(Block* block) {
  for (const auto& edge : block->preds()) {
    if (edge->type() == cfg::EDGE_GHOST) {
      return true;
    }
  }
  return false;
}

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
          const auto* error_val = error_val_opt ? &*error_val_opt : nullptr;
          parser_state.emplace_back(std::move(expr_stack), root_expr_is_nil,
                                    /* default_val */ nullptr, error_val);
          break;
        }

      case 2: {
        // A default Val.
        const auto* default_val = &std::get<2>(p);
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

    auto val = start_profile(cur, false);

    source_blocks::impl::BlockAccessor::push_source_block(
        cur, std::make_unique<SourceBlock>(method, id, val));
    ++id;

    if (insert_after_excs) {
      insert_after_exceptions_impl(
          cur, method, id, serialize, oss, [this, cur]() {
            return start_profile(cur, /*empty_inner_tail=*/true);
          });
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
      if (p_state.default_val != nullptr) {
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
      auto val = p_state.error_val != nullptr ? *p_state.error_val
                                              : SourceBlock::Val::none();
      for (auto* b : cfg.blocks()) {
        auto vec = gather_source_blocks(b);
        for (auto* sb : vec) {
          const_cast<SourceBlock*>(sb)->set_at(i, val);
        }
      }
    }
    return ret;
  }
};

struct CustomValueInsertHelper {

  enum class CustomInsertionType { DEFAULT_VALUES = 0, FUZZING_VALUES = 1 };

  std::ostringstream oss;
  const DexString* method;
  uint32_t id{0};
  bool serialize;
  bool insert_after_excs;
  CustomInsertionType insertion_type;
  size_t interaction_size;
  float cold_to_hot_generation_ratio{0.2f};
  float hot_throw_cold_generation_ratio{0.01f};

  CustomValueInsertHelper(const DexString* method,
                          const std::vector<ProfileData>& profiles,
                          bool serialize,
                          bool insert_after_excs,
                          CustomInsertionType insertion_type)
      : method(method),
        serialize(serialize),
        insert_after_excs(insert_after_excs),
        insertion_type(insertion_type),
        interaction_size(profiles.size()) {}

  struct FuzzingMetadata {
    uint32_t indegrees{0};
    uint32_t insertion_id{0};
    bool has_values{false}; // to check if a block has had it's values filled in
    bool should_be_hot{false}; // to check if a block should be set as hot, used
                               // if a pred is hot and wants to set at least one
                               // succ to hot. If this false, the source block
                               // can be either hot or cold

    [[maybe_unused]] FuzzingMetadata() = default;

    FuzzingMetadata(uint32_t indegrees, uint32_t insertion_id)
        : indegrees(indegrees), insertion_id(insertion_id) {}

    bool operator<(const FuzzingMetadata& r) const {
      if (indegrees == r.indegrees) {
        return insertion_id < r.insertion_id;
      }
      return indegrees < r.indegrees;
    }
  };

  UnorderedMap<Block*, FuzzingMetadata> fuzzing_metadata_map;

  std::vector<SourceBlock::Val> generate_default_data_for_fuzzing(
      Block* /*b*/) {
    // sets all blocks' values to (-1|-1)
    std::vector<SourceBlock::Val> interaction_pairs;
    interaction_pairs.reserve(interaction_size);

    for (size_t i = 0; i < interaction_size; ++i) {
      float hit = -1;
      float appear100 = -1;
      interaction_pairs.emplace_back(hit, appear100);
    }

    return interaction_pairs;
  }

  std::vector<SourceBlock::Val> start_profile(Block* cur) {
    std::vector<SourceBlock::Val> ret;
    bool use_fuzzing = (insertion_type == CustomInsertionType::FUZZING_VALUES);
    std::vector<SourceBlock::Val> fuzzing_values;
    if (use_fuzzing) {
      fuzzing_values = generate_default_data_for_fuzzing(cur);
    }
    ret.reserve(interaction_size);

    for (size_t i = 0; i < interaction_size; ++i) {
      const auto& value_to_insert =
          use_fuzzing ? fuzzing_values.at(i) : global_default_val;
      ret.emplace_back(value_to_insert);
    }
    return ret;
  }

  void start(Block* cur) {
    if (serialize) {
      oss << "(" << id;
    }

    if (fuzzing_metadata_map.find(cur) == fuzzing_metadata_map.end()) {
      fuzzing_metadata_map.insert({cur, FuzzingMetadata(0, 0)});
    }

    auto val = start_profile(cur);

    source_blocks::impl::BlockAccessor::push_source_block(
        cur, std::make_unique<SourceBlock>(method, id, val));
    ++id;

    if (insert_after_excs) {
      insert_after_exceptions_impl(
          cur, method, id, serialize, oss,
          [this, cur]() { return start_profile(cur); });
    }
  }

  void edge(Block* /*cur*/, const Edge* e) {
    if (serialize) {
      oss << " " << get_edge_char(e);
    }
    Block* target = e->target();
    fuzzing_metadata_map.try_emplace(target, 0, 0).first->second.indegrees++;
  }

  void end(Block* /*cur*/) {
    if (serialize) {
      oss << ")";
    }
  }
};

struct RandomGenerator {
  std::random_device dev;
  std::mt19937 random_number_generator;
  std::uniform_real_distribution<> uniform_distribution;
  float cold_to_hot_generation_ratio{0.5f};
  float hot_throw_cold_generation_ratio{0.01f};

  RandomGenerator() = delete;

  explicit RandomGenerator(float cold_to_hot_generation_ratio,
                           float hot_throw_cold_generation_ratio = 0.05f)
      : random_number_generator(dev()),
        uniform_distribution(0.0, 1.0),
        cold_to_hot_generation_ratio(cold_to_hot_generation_ratio),
        hot_throw_cold_generation_ratio(hot_throw_cold_generation_ratio) {}

  explicit RandomGenerator(int seed,
                           const std::string& method_name,
                           float cold_to_hot_generation_ratio,
                           float hot_throw_cold_generation_ratio = 0.05f)
      : random_number_generator(seed ^ std::hash<std::string>{}(method_name)),
        uniform_distribution(0.0, 1.0),
        cold_to_hot_generation_ratio(cold_to_hot_generation_ratio),
        hot_throw_cold_generation_ratio(hot_throw_cold_generation_ratio) {}

  float generate_appear100() {
    return static_cast<float>(uniform_distribution(random_number_generator));
  }

  float generate_block_hit() {
    float hit_percent =
        static_cast<float>(uniform_distribution(random_number_generator));
    if (hit_percent <= cold_to_hot_generation_ratio) {
      return 0.0;
    } else {
      return 1.0;
    }
  }

  // This generates if a block should be converted from hot -> throw -> hot to
  // hot -> throw -> cold
  bool generate_if_convert_hot_throw_cold() {
    float convert_percent =
        static_cast<float>(uniform_distribution(random_number_generator));
    return (convert_percent <= hot_throw_cold_generation_ratio);
  }

  // generates a random size_t between [start, end] inclusive
  size_t generate_random_number_in_range(int start, int end) {
    return start + (size_t)((uniform_distribution(random_number_generator)) *
                            ((end - start) + 1));
  }
};

bool is_hot_block(const Block* block) {
  return has_source_block_positive_val(get_last_source_block(block));
}

void set_block_appear100(Block* block, float appear100) {
  std::vector<SourceBlock*> source_blocks = gather_source_blocks(block);
  for (auto* source_block : source_blocks) {
    source_block->foreach_val([&](auto& val) {
      if (val) {
        val->appear100 = appear100;
      }
    });
  }
}

void set_block_value(Block* block, float hit) {
  std::vector<SourceBlock*> source_blocks = gather_source_blocks(block);
  for (auto* source_block : source_blocks) {

    source_block->foreach_val([&](auto& val) {
      if (val) {
        val->val = hit;
      }
    });
  }
}

int get_number_of_throws_in_block(Block* curr) {
  int count{0};
  for (auto& mie : *curr) {
    if (mie.type == MFLOW_OPCODE && opcode::can_throw(mie.insn->opcode())) {
      count++;
    }
  }
  return count;
}

void set_source_block_value(SourceBlock* source_block, float hit) {
  for (auto* sb = source_block; sb != nullptr; sb = sb->next.get()) {
    sb->foreach_val([&](auto& val) {
      if (val) {
        val->val = hit;
      }
    });
  }
}

struct TopoTraversalHelper {

  CustomValueInsertHelper* value_insert_helper;
  RandomGenerator* generator;
  dominators::SimpleFastDominators<cfg::GraphInterface>* doms;
  ControlFlowGraph* cfg;

  float appear100{0.0f};
  float entry_hit{0.0f};

  explicit TopoTraversalHelper(
      CustomValueInsertHelper* value_insert_helper,
      RandomGenerator* generator,
      dominators::SimpleFastDominators<cfg::GraphInterface>* doms,
      ControlFlowGraph* cfg)
      : value_insert_helper(value_insert_helper),
        generator(generator),
        doms(doms),
        cfg(cfg) {}

  bool all_preds_are_cold(const Block* block) {
    auto& metadata = value_insert_helper->fuzzing_metadata_map;
    for (const auto& edge : block->preds()) {
      auto* pred = edge->src();
      if (!metadata.at(pred).has_values) {
        // If there are no values in this predecessor (cause of cycles) then
        // ignore it
        continue;
      }
      if (is_hot_block(pred)) {
        return false;
      }
    }
    return true;
  }

  // This function takes a block that has already been chosen to be set as hot
  // -> throw -> cold. Since there may be more than 1 throw instruction in a
  // block, this will chose a random throw, and all source blocks from that
  // throw onwards will be cold
  void set_block_as_hot_throw_cold(Block* block, int throw_count) {
    size_t throw_to_convert =
        generator->generate_random_number_in_range(0, throw_count - 1);
    size_t current_throw = 0;
    bool set_hot = true;

    for (auto& mie : *block) {
      if (mie.type == MFLOW_OPCODE && opcode::can_throw(mie.insn->opcode())) {
        if (current_throw >= throw_to_convert) {
          set_hot = false;
        }
        current_throw++;
      }

      if (mie.type == MFLOW_SOURCE_BLOCK) {
        if (set_hot) {
          set_source_block_value(mie.src_block.get(), 1);
        } else {
          set_source_block_value(mie.src_block.get(), 0);
        }
      }
    }
  }

  void set_block_random_value(Block* block) {
    int next_hit = static_cast<int>(generator->generate_block_hit());
    int throw_count = get_number_of_throws_in_block(block);

    if (next_hit == 0 || throw_count == 0) {
      set_block_value(block, static_cast<float>(next_hit));
    } else {
      bool should_convert_to_hot_throw_cold =
          generator->generate_if_convert_hot_throw_cold();
      if (should_convert_to_hot_throw_cold) {
        set_block_as_hot_throw_cold(block, throw_count);
      } else {
        set_block_value(block, static_cast<float>(next_hit));
      }
    }
  }

  // This assumes the last source block is HOT (and has no throws after it), and
  // sets a random child to be HOT
  void set_succ_randomly_hot(Block* block) {
    auto& metadata = value_insert_helper->fuzzing_metadata_map;
    const auto& successors = block->succs();
    size_t successor_size = successors.size();
    size_t next_hot_block_idx = generator->generate_random_number_in_range(
        0, static_cast<int>(successor_size - 1));

    for (size_t i = 0; i < successor_size; ++i) {
      auto* succ_block = successors.at(i)->target();

      if (next_hot_block_idx == i) {
        metadata.at(succ_block).should_be_hot = true;
      }
    }
  }

  void process_block(Block* cur, bool must_be_cold) {
    auto& metadata = value_insert_helper->fuzzing_metadata_map;
    if (cur == cfg->entry_block()) {
      entry_hit = generator->generate_block_hit();
      if (must_be_cold) {
        entry_hit = 0.0;
      }
      appear100 = 0.0;
      if (entry_hit > 0) { // entry_hit is HOT.
        appear100 = generator->generate_appear100();
      }
      set_block_appear100(cur, appear100);
      set_block_value(cur, entry_hit);
      metadata.at(cur).has_values = true;
    } else {
      if (!metadata.at(cur).has_values) {
        // if a block has values, don't need to deal with it again
        // (for example, the entry block).
        if (entry_hit == 0.0f || !is_hot_block(doms->get_idom(cur)) ||
            all_preds_are_cold(cur)) {
          // This block must be listed as COLD now.
          set_block_value(cur, 0);
        } else {
          if (metadata.at(cur).should_be_hot) {
            // This block must be set to HOT now
            set_block_value(cur, 1);
          } else {
            // This block can be either HOT or COLD.
            set_block_random_value(cur);
          }
        }
        set_block_appear100(cur, appear100);
        metadata.at(cur).has_values = true;
      }
    }

    auto* last_source_block_or_throw =
        get_last_source_block_if_after_throw(cur);
    // This is to check if there is a throw after a source block, if there is a
    // throw, last_source_block_or_throw is null, and therefore the successors
    // can be COLD. Otherwise, if there exists a source block at the end of this
    // block and it is hot, then the hot path must continue into at least one
    // successor
    if ((last_source_block_or_throw != nullptr) &&
        has_source_block_positive_val(last_source_block_or_throw)) {
      set_succ_randomly_hot(cur);
    }
  }
};

using InsertionType = CustomValueInsertHelper::CustomInsertionType;

} // namespace

SourceBlockConsistencyCheck& get_sbcc() { return s_sbcc; }

InsertResult insert_source_blocks(DexMethod* method,
                                  ControlFlowGraph* cfg,
                                  const std::vector<ProfileData>& profiles,
                                  bool serialize,
                                  bool insert_after_excs) {
  return insert_source_blocks(&method->get_deobfuscated_name(), cfg, profiles,
                              serialize, insert_after_excs);
}

static std::string get_serialized_idom_map(ControlFlowGraph* cfg) {
  auto doms = dominators::SimpleFastDominators<cfg::GraphInterface>(*cfg);
  std::stringstream ss_idom_map;
  auto cfg_blocks = cfg->blocks();
  bool wrote_first_idom_map_elem = false;
  auto write_idom_map_elem = [&wrote_first_idom_map_elem,
                              &ss_idom_map](uint32_t sb_id, uint32_t idom_id) {
    if (wrote_first_idom_map_elem) {
      ss_idom_map << ";";
    }
    wrote_first_idom_map_elem = true;

    ss_idom_map << sb_id << "->" << idom_id;
  };

  for (cfg::Block* block : cfg->blocks()) {
    if (block == cfg->exit_block() &&
        (cfg->get_pred_edge_of_type(block, EDGE_GHOST) != nullptr)) {
      continue;
    }

    auto* first_sb_in_block = source_blocks::get_first_source_block(block);
    if (first_sb_in_block == nullptr) {
      continue;
    }

    auto* curr_idom = doms.get_idom(block);
    if ((curr_idom != nullptr) && curr_idom != block) {
      if (curr_idom != cfg->exit_block() ||
          (cfg->get_pred_edge_of_type(curr_idom, EDGE_GHOST) == nullptr)) {
        auto* sb_in_idom = source_blocks::get_last_source_block(curr_idom);
        always_assert(sb_in_idom);
        write_idom_map_elem(first_sb_in_block->id, sb_in_idom->id);
      }
    }

    SourceBlock* prev = nullptr;
    source_blocks::foreach_source_block(block, [&](const auto& sb) {
      if (sb != first_sb_in_block) {
        always_assert(prev);
        write_idom_map_elem(sb->id, prev->id);
      }
      prev = sb;
    });
  }

  return ss_idom_map.str();
}

std::tuple<size_t, size_t, size_t, size_t> normalize_blocks(
    ControlFlowGraph* cfg) {
  size_t normalized_blocks = 0;
  size_t denormalized_blocks = 0;
  size_t elided_vals = 0;
  size_t unelided_vals = 0;
  for (auto* block : cfg->blocks()) {
    source_blocks::foreach_source_block(block, [&](const auto& sb) {
      if (sb->normalize(&elided_vals, &unelided_vals)) {
        normalized_blocks++;
      } else {
        denormalized_blocks++;
      }
    });
  }
  return std::make_tuple(normalized_blocks, denormalized_blocks, elided_vals,
                         unelided_vals);
}

InsertResult insert_source_blocks(const DexString* method,
                                  ControlFlowGraph* cfg,
                                  const std::vector<ProfileData>& profiles,
                                  bool serialize,
                                  bool insert_after_excs) {
  InsertHelper helper(method, profiles, serialize, insert_after_excs);

  impl::visit_in_order(
      cfg,
      [&](Block* cur) { helper.start(cur); },
      [&](Block* cur, const Edge* e) { helper.edge(cur, e); },
      [&](Block* cur) { helper.end(cur); });

  bool had_failures = helper.wipe_profile_failures(*cfg);

  auto idom_map = get_serialized_idom_map(cfg);

  auto [normalized_blocks, denormalized_blocks, elided_vals, unelided_vals] =
      normalize_blocks(cfg);
  return {helper.id,     helper.oss.str(),  std::move(idom_map),
          !had_failures, normalized_blocks, denormalized_blocks,
          elided_vals,   unelided_vals};
}

// This metric checks if there exists a block such that it starts out with hot
// source blocks, encounters a throw instruction somewhere, and then has cold
// source blocks. It does this by first checking the first source block is hot,
// and then checking if there has been a throw, then seeing if any subsequent
// source block is cold.
bool block_is_hot_throw_cold(Block* cur) {
  bool seen_src_block = false;
  bool seen_throw = false;
  for (auto& mie : *cur) {
    if (mie.type == MFLOW_OPCODE && opcode::can_throw(mie.insn->opcode())) {
      seen_throw = true;
    }

    if (mie.type == MFLOW_SOURCE_BLOCK) {
      if (!seen_src_block) {
        // Cold initial source blocks do not count here
        if (!has_source_block_positive_val(mie.src_block.get())) {
          return false;
        }
        // If a throw has been seen before the initial source block, this also
        // does not count
        if (seen_throw) {
          return false;
        }
        seen_src_block = true;
      } else {
        // This means a throw has been seen, and the current source block is
        // cold while the initial one is hot
        if (seen_throw && !has_source_block_positive_val(mie.src_block.get())) {
          return true;
        }
      }
    }
  }
  return false;
}

void update_source_block_metric(Block* cur, SourceBlockMetric& metrics) {
  auto source_blocks = gather_source_blocks(cur);
  for (const auto& source_block : source_blocks) {
    if (has_source_block_positive_val(source_block)) {
      metrics.hot_block_count++;
    } else {
      metrics.cold_block_count++;
    }
  }

  if (block_is_hot_throw_cold(cur)) {
    metrics.hot_throw_cold_count++;
  }
}

SourceBlockMetric gather_source_block_metrics(ControlFlowGraph* cfg) {
  SourceBlockMetric metrics = {0, 0, 0};

  impl::visit_in_order(
      cfg,
      [&](Block* cur) { update_source_block_metric(cur, metrics); },
      [&](Block* /*cur*/, const Edge* /*e*/) {},
      [&](Block* /*cur*/) {});

  return metrics;
}

// CustomValueInsertHelper must have the correct indegrees initialized to have
// this work as intended. Running this traversal will also modify the state of
// the CustomValueInsertHelper to have different indegrees
template <typename BlockFn>
void topo_traverse(CustomValueInsertHelper& helper,
                   Block* start_block,
                   const BlockFn& block_fn) {
  auto& metadata = helper.fuzzing_metadata_map;
  auto topo_comparator = [&metadata](Block* l, Block* r) {
    return metadata.at(l) < metadata.at(r);
  };

  // The traversal uses a multiset to be able to process the block with the
  // lowest number of indegrees, and allows updating the position of other nodes
  // in the multiset if their indegrees change during the traversal.
  std::multiset<Block*, decltype(topo_comparator)> process_queue(
      topo_comparator);
  UnorderedSet<Block*> visited;
  uint32_t insertion_order_id = 0;

  block_fn(start_block);
  metadata.at(start_block).insertion_id = insertion_order_id;
  visited.insert(start_block);
  process_queue.insert(start_block);
  ++insertion_order_id;

  while (!process_queue.empty()) {
    auto current = process_queue.begin();
    process_queue.erase(process_queue.begin());

    block_fn(*current);

    for (const auto& edge : (*current)->succs()) {
      auto* neighbor = edge->target();
      bool re_add_neighbor = false;

      if (process_queue.count(neighbor)) {
        // Erasing from the queue and readding allows reordering and updating of
        // the queue if the neighbor's new indegrees gives it more priority now
        process_queue.erase(neighbor);
        re_add_neighbor = true;
      }
      metadata.at(neighbor).indegrees--;
      if (re_add_neighbor) {
        process_queue.insert(neighbor);
      }

      if (!visited.count(neighbor)) {
        metadata.at(neighbor).insertion_id = insertion_order_id;
        ++insertion_order_id;
        visited.insert(neighbor);
        process_queue.insert(neighbor);
      }
    }
  }
}

/*
The follow method runs a topo-traversal of the method's CFG, filling out source
blocks as it goes.

Starting from the entry block, each block gets processed based on the number of
indegrees it has. The lower the number of indegrees, the more priority a block
has. Therefore, a multiset is used as the processing queue because it allows
easy retrieval of the block with the lowest indegrees (stored in the metadata),
and allows updating current blocks and modifying their processing order inside
the processing queue. There might be ties in indegrees, if so, the block that
was first inserted among the tied ones is processed first.

There are a few helpers generated, including a dominator tree to find the
immediate dominator quickly, and a Random Generator to generate fuzzing data.

There are in general a few cases while filling out source block values during
the traversal:

1) In order to determine a source block's values, its predecessor and immediate
dominator values must be known, therefore the traversal must visit and fill
their values out first. This is also why the traversal uses indegrees to
traverse, as it will traverse both the predecessors and immediate dominator of a
block before getting to it.

2) if the immediate dominator is cold OR if all predecessors are cold OR if the
entry block is cold, then the block must be cold, otherwise can be either hot or
cold.

3) all source blocks should have the same appear100 value in the method.
*/
void run_topotraversal_fuzzing(ControlFlowGraph* cfg,
                               CustomValueInsertHelper& helper,
                               const std::string& method_name,
                               bool use_seed,
                               int seed,
                               bool must_be_cold) {

  RandomGenerator generator =
      use_seed ? RandomGenerator(seed, method_name,
                                 helper.cold_to_hot_generation_ratio,
                                 helper.hot_throw_cold_generation_ratio)
               : RandomGenerator(helper.cold_to_hot_generation_ratio,
                                 helper.hot_throw_cold_generation_ratio);
  auto doms = dominators::SimpleFastDominators<cfg::GraphInterface>(*cfg);
  TopoTraversalHelper traversal_helper(&helper, &generator, &doms, cfg);

  topo_traverse(helper, cfg->entry_block(), [&](Block* cur) {
    traversal_helper.process_block(cur, must_be_cold);
  });
}

InsertResult insert_custom_source_blocks(
    const DexString* method,
    ControlFlowGraph* cfg,
    const std::vector<ProfileData>& profiles,
    bool serialize,
    bool insert_after_excs,
    bool enable_fuzzing,
    bool must_be_cold) {
  // If fuzzing is on and must_be_cold is true, then the method's entry hit
  // should be cold, otherwise it should be randomly generated by the
  // RandomGenerator

  InsertionType insertion_type = InsertionType::DEFAULT_VALUES;
  if (enable_fuzzing) {
    insertion_type = InsertionType::FUZZING_VALUES;
  }
  int seed = 5000;
  bool use_seed = true;
  CustomValueInsertHelper helper(method, profiles, serialize, insert_after_excs,
                                 insertion_type);
  impl::visit_in_order(
      cfg,
      [&](Block* cur) { helper.start(cur); },
      [&](Block* cur, const Edge* e) { helper.edge(cur, e); },
      [&](Block* cur) { helper.end(cur); });

  if (insertion_type == InsertionType::FUZZING_VALUES) {
    run_topotraversal_fuzzing(cfg, helper, std::string(method->str()), use_seed,
                              seed, must_be_cold);
  }

  auto idom_map = get_serialized_idom_map(cfg);

  auto [normalized_blocks, denormalized_blocks, elided_vals, unelided_vals] =
      normalize_blocks(cfg);

  return {helper.id,   helper.oss.str(),  std::move(idom_map),
          true,        normalized_blocks, denormalized_blocks,
          elided_vals, unelided_vals};
}

UnorderedMap<Block*, uint32_t> insert_custom_source_blocks_get_indegrees(
    const DexString* method,
    ControlFlowGraph* cfg,
    const std::vector<ProfileData>& profiles,
    bool serialize,
    bool insert_after_excs,
    bool enable_fuzzing) {

  InsertionType insertion_type = InsertionType::DEFAULT_VALUES;
  if (enable_fuzzing) {
    insertion_type = InsertionType::FUZZING_VALUES;
  }
  CustomValueInsertHelper helper(method, profiles, serialize, insert_after_excs,
                                 insertion_type);
  impl::visit_in_order(
      cfg,
      [&](Block* cur) { helper.start(cur); },
      [&](Block* cur, const Edge* e) { helper.edge(cur, e); },
      [&](Block* cur) { helper.end(cur); });

  UnorderedMap<Block*, uint32_t> indegrees;
  for (auto& entry : UnorderedIterable(helper.fuzzing_metadata_map)) {
    Block* cur = entry.first;
    CustomValueInsertHelper::FuzzingMetadata& metadata = entry.second;
    indegrees.emplace(cur, metadata.indegrees);
  }
  return indegrees;
}

void scale_source_blocks(cfg::Block* block) {
  // After duplicating hot successors, some originally hot blocks may no
  // longer be reachable from hot blocks. We'll mark them as cold here.

  // Note that while the currently implemented "scaling" approach via min/max
  // works well when we only consider binary hit-count state values (0 = cold,
  // >0 = hot), and that it doesn't do actual proper scaling yet. A practical
  // problem for that is that we don't know the hotness of edges, and thus
  // cannot correctly attribute hit-counts to the original or duplicated blocks
  // outside of the binary scenario.
  // TODO: Implement proper numeric scaling. This will be especially important
  // once we track (non-binary) block hit counts.

  // Note that we don't have to further deal with dependencies as we are
  // iterating from the front, and have filtered out back-edges.
  auto* template_sb = source_blocks::get_first_source_block(block);
  always_assert(template_sb);
  SourceBlock limit_sb(*template_sb);
  limit_sb.fill(SourceBlock::Val(0, 0));

  for (auto* pred : block->preds()) {
    if (pred->src() == nullptr) {
      continue;
    }

    auto* pred_sb = source_blocks::get_last_source_block(pred->src());
    if (pred_sb == nullptr) {
      // Missing information; give up
      return;
    }

    // `SourceBlock::max` is a bit dubious, as the appear100 values really
    // represent a set
    limit_sb.max(*pred_sb);
  }
  source_blocks::foreach_source_block(block,
                                      [&](auto* sb) { sb->min(limit_sb); });
}

static void process_source_blocks_for_violations(SourceBlock* first_sb,
                                                 std::vector<bool>& hit_tracker,
                                                 bool update_hit_before_fix) {
  for (auto* sb = first_sb; sb != nullptr; sb = sb->next.get()) {
    for (size_t i = 0; i < sb->vals_size; i++) {
      always_assert(i < hit_tracker.size());
      bool is_hit = sb->get_val(i).value_or(0) > 0;

      if (update_hit_before_fix && is_hit) {
        hit_tracker[i] = true;
      }

      if (hit_tracker[i] && sb->get_val(i).value_or(0) <= 0) {
        sb->set_at(i, SourceBlock::Val(1, sb->get_appear100(i).value_or(1)));
      }

      if (!update_hit_before_fix && is_hit) {
        hit_tracker[i] = true;
      }
    }
  }
}

void fix_chain_violations(ControlFlowGraph* cfg) {
  impl::visit_in_order(
      cfg,
      [&](Block* cur) {
        uint32_t vals_size =
            source_blocks::get_first_source_block(cur)->vals_size;
        // Iterate over the source blocks in reverse order
        // If a source block is hit for an interaction, then all
        // source blocks preceding it must be hit for that interaction
        std::vector<bool> any_hit_rev(vals_size, false);
        for (auto mie_it = cur->rbegin(); mie_it != cur->rend(); mie_it++) {
          auto& mie = *mie_it;
          if (mie.type == MFLOW_SOURCE_BLOCK) {
            process_source_blocks_for_violations(
                mie.src_block.get(), any_hit_rev,
                /* update_hit_before_fix= */ false);
          }
        }
        // Iterate over the source blocks in forward order
        // If a source block is hit for an interaction, then all
        // source blocks after it must be hit for that interaction
        // unless it is separated by a throwing instruction
        std::vector<bool> any_hit_for(vals_size, false);
        for (auto& mie : *cur) {
          if (mie.type == MFLOW_OPCODE &&
              opcode::can_throw(mie.insn->opcode())) {
            std::fill(any_hit_for.begin(), any_hit_for.end(), false);
          } else if (mie.type == MFLOW_SOURCE_BLOCK) {
            process_source_blocks_for_violations(
                mie.src_block.get(), any_hit_for,
                /* update_hit_before_fix= */ true);
          }
        }
      },
      [&](Block* /* cur */, const Edge* /* e */) {}, [&](Block* /* cur */) {});
}

void fix_idom_violation(
    Block* cur,
    uint32_t vals_index,
    const dominators::SimpleFastDominators<cfg::GraphInterface>& dom) {
  bool first_source_block_changed = true;
  if (source_blocks::get_first_source_block(cur)
          ->get_val(vals_index)
          .value_or(0) > 0) {
    first_source_block_changed = false;
  }
  foreach_source_block(cur, [&](auto& sb) {
    if (sb->get_val(vals_index).value_or(0) <= 0) {
      sb->set_at(
          vals_index,
          SourceBlock::Val(1, sb->get_appear100(vals_index).value_or(1)));
    }
  });
  auto* idom = dom.get_idom(cur);
  if (first_source_block_changed && idom != cur) {
    fix_idom_violation(idom, vals_index, dom);
  }
}

void fix_idom_violations(ControlFlowGraph* cfg) {
  dominators::SimpleFastDominators<cfg::GraphInterface> dom{*cfg};
  impl::visit_in_order(
      cfg,
      [&](Block* cur) {
        auto* first_sb = source_blocks::get_first_source_block(cur);
        auto* idom = dom.get_idom(cur);
        if (idom != cur) {
          uint32_t vals_size = first_sb->vals_size;
          for (uint32_t i = 0; i < vals_size; i++) {
            if (first_sb->get_val(i).value_or(0) > 0) {
              fix_idom_violation(idom, i, dom);
            }
          }
        }
      },
      [&](Block* /* cur */, const Edge* /* e */) {}, [&](Block* /* cur */) {});
}

void fix_hot_method_cold_entry_violations(ControlFlowGraph* cfg) {
  auto* entry_block = cfg->entry_block();
  if (entry_block == nullptr) {
    return;
  }
  auto* sb = get_first_source_block(entry_block);
  if (sb == nullptr) {
    return;
  }
  uint32_t vals_size = sb->vals_size;
  for (uint32_t i = 0; i < vals_size; i++) {
    if (sb->get_val(i).value_or(0) <= 0 &&
        sb->get_appear100(i).value_or(0) > 0) {
      sb->set_at(i, SourceBlock::Val(1, sb->get_appear100(i).value_or(1)));
    }
  };
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
  // Do not put source blocks before a (pseudo) move result or load-param-* at
  // the head of a block.
  auto can_insert_source_block_before = [](IROpcode op) {
    return !opcode::is_a_load_param(op) && !opcode::is_move_result_any(op) &&
           !opcode::is_move_exception(op);
  };
  auto it = b->begin();
  while (it != b->end() && it->type == MFLOW_OPCODE &&
         !can_insert_source_block_before(it->insn->opcode())) {
    ++it;
  }
  return it;
}

namespace normalize {

size_t num_interactions(const cfg::ControlFlowGraph& cfg,
                        const SourceBlock* sb) {
  if (g_redex != nullptr) {
    return g_redex->num_sb_interaction_indices();
  }

  auto nums = [](auto* sb) -> std::optional<size_t> {
    return (sb != nullptr) ? std::optional<size_t>(sb->vals_size)
                           : std::nullopt;
  };

  auto caller = nums(sb);
  if (caller) {
    return *caller;
  }
  auto callee = nums(source_blocks::get_first_source_block(cfg.entry_block()));
  if (callee) {
    return *callee;
  }

  return 0;
}

} // namespace normalize

namespace {

size_t count_blocks(
    Block*, const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  return 1;
}
size_t count_block_has_sbs(
    Block* b, const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  return source_blocks::has_source_blocks(b) ? 1 : 0;
}
size_t count_block_has_incomplete_sbs(
    Block* b, const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  auto* sb = get_first_source_block(b);
  if (sb == nullptr) {
    return 0;
  }
  return static_cast<size_t>(
      sb->foreach_val_early([&](const auto& val) { return !val; }));
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

  auto* immediate_dominator = dominators.get_idom(block);
  if (immediate_dominator == nullptr) {
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

  if (block->preds().empty()) {
    return 0;
  }

  for (auto* predecessor : block->preds()) {
    auto* first_sb_pred =
        source_blocks::get_first_source_block(predecessor->src());
    if (has_source_block_positive_val(first_sb_pred)) {
      return 0;
    }
  }
  return 1;
}

size_t hot_all_children_cold(Block* block) {
  auto* last_sb_before_throw =
      source_blocks::get_last_source_block_if_after_throw(block);

  if ((last_sb_before_throw == nullptr) ||
      !has_source_block_positive_val(last_sb_before_throw)) {
    return 0;
  }

  bool has_successor = false;
  for (auto* successor : block->succs()) {
    auto* first_sb_succ =
        source_blocks::get_first_source_block(successor->src());
    has_successor = true;
    // This means that for this current hot block (with respect to the last
    // source block of the hot block), there exists at least one successor
    // that is hot, therefore this not a violation
    if (has_source_block_positive_val(first_sb_succ)) {
      return 0;
    }
  }
  // Only blocks with successors should have this count as a violation
  return has_successor ? 1 : 0;
}

size_t hot_callee_all_cold_callers(call_graph::NodeId node,
                                   SourceBlockInvokeMap& src_block_invoke_map) {
  // Ignore Ghost Nodes
  if (node->is_entry() || node->is_exit()) {
    return 0;
  }

  auto* callee_method = const_cast<DexMethod*>(node->method());
  if (callee_method == nullptr || callee_method->get_code() == nullptr) {
    return 0;
  }

  ScopedCFG callee_cfg(callee_method->get_code());

  // Cold Callees do not count
  auto* callee_entry_block = get_first_source_block(callee_cfg->entry_block());
  if ((callee_entry_block == nullptr) || callee_entry_block->vals_size == 0 ||
      callee_entry_block->get_val(0).value_or(0) == 0) {
    return 0;
  }

  for (const auto* caller_edge : node->callers()) {
    const auto* caller = caller_edge->caller();
    // If a node is connected to the ghost entry node, we should not count it
    // as a violation because we can treat a ghost node's transition to its
    // successors as hot
    if (caller->is_entry()) {
      return 0;
    }

    auto* caller_method = const_cast<DexMethod*>(caller->method());
    if (caller_method == nullptr || caller_method->get_code() == nullptr) {
      continue;
    }

    auto* invoke_insn = caller_edge->invoke_insn();
    // TODO(T229471397): With multiple-callee graphs, there might be more
    // accurate way to check which specific method is calling the callee
    // (method override check)

    if (src_block_invoke_map.count(caller_method) == 0) {
      continue;
    }
    const auto& source_block_bools_before_invoke =
        src_block_invoke_map.at_unsafe(caller_method)[invoke_insn];
    for (auto b : source_block_bools_before_invoke) {
      if (b) {
        return 0;
      }
    }
  }
  return 1;
}

// Helper transforms for violation counting
namespace {
constexpr auto identity_transform = [](auto val) { return val; };
constexpr auto binary_transform = [](auto val) { return val > 0 ? 1 : 0; };
} // namespace

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

template <typename Fn>
size_t hot_method_cold_entry_violations_tmpl(Block* block, const Fn& fn) {
  size_t sum{0};
  if (block->preds().empty()) {
    auto* sb = get_first_source_block(block);
    if (sb != nullptr) {
      sb->foreach_val([&sum](const auto& val) {
        if (val && val->appear100 != 0 && val->val == 0) {
          sum++;
        }
      });
    }
  }
  sum = fn(sum);
  return sum;
}

template <typename Fn>
size_t hot_all_children_cold_violations_tmpl(Block* block, const Fn& fn) {
  size_t sum{0};

  auto* last_sb_before_throw =
      source_blocks::get_last_source_block_if_after_throw(block);

  if (last_sb_before_throw &&
      has_source_block_positive_val(last_sb_before_throw)) {
    bool has_successor = false;
    bool has_cold_child = true;
    for (auto* successor : block->succs()) {
      auto* first_sb_succ =
          source_blocks::get_first_source_block(successor->src());
      has_successor = true;
      if (has_source_block_positive_val(first_sb_succ)) {
        has_cold_child = false;
        break;
      }
    }
    if (has_successor && has_cold_child) {
      sum++;
    }
  }
  sum = fn(sum);
  return sum;
}

size_t chain_hot_violations(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  return chain_hot_violations_tmpl(block, identity_transform);
}

size_t chain_hot_one_violations(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  return chain_hot_violations_tmpl(block, binary_transform);
}

size_t hot_method_cold_entry_violations(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  return hot_method_cold_entry_violations_tmpl(block, identity_transform);
}

size_t hot_method_cold_entry_block_violations(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  return hot_method_cold_entry_violations_tmpl(block, binary_transform);
}

size_t hot_all_children_cold_violations(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>&) {
  return hot_all_children_cold_violations_tmpl(block, identity_transform);
};

struct ChainAndDomState {
  const SourceBlock* last{nullptr};
  cfg::Block* dom_block{nullptr};
  size_t violations{0};
};

template <uint32_t kMaxInteraction>
void chain_and_dom_update(
    cfg::Block* block,
    const SourceBlock* sb,
    bool first_in_block,
    bool prev_insn_can_throw,
    ChainAndDomState& state,
    const dominators::SimpleFastDominators<cfg::GraphInterface>& dom) {
  if (first_in_block) {
    state.last = nullptr;
    for (auto* b = dom.get_idom(block); state.last == nullptr && b != nullptr;
         b = dom.get_idom(b)) {
      if (b == block) {
        state.dom_block = nullptr;
        break;
      }
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
    bool cold_precedes_hot =
        is_less_than_for_any_value(state.last, sb, kMaxInteraction);
    bool hot_precedes_cold =
        is_less_than_for_any_value(sb, state.last, kMaxInteraction) &&
        !first_in_block && !prev_insn_can_throw;
    if (cold_precedes_hot || hot_precedes_cold) {
      state.violations++;
    }
  }

  state.last = sb;
}

template <uint32_t kMaxInteraction>
size_t chain_and_dom_violations_impl(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>& dom) {
  ChainAndDomState state{};
  bool first = true;
  // True if any instruction that we've encountered since the last source
  // block can throw
  bool prev_insn_can_throw = false;
  for (const auto& mie : *block) {
    switch (mie.type) {
    case MFLOW_OPCODE:
      prev_insn_can_throw =
          prev_insn_can_throw || opcode::can_throw(mie.insn->opcode());
      break;
    case MFLOW_SOURCE_BLOCK:
      for (auto* sb = mie.src_block.get(); sb != nullptr; sb = sb->next.get()) {
        chain_and_dom_update<kMaxInteraction>(block, sb, first,
                                              prev_insn_can_throw, state, dom);
        first = false;
        prev_insn_can_throw = false;
      }
      break;
    default:
      break;
    }
  }

  return state.violations;
}

size_t chain_and_dom_violations(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>& dom) {
  return chain_and_dom_violations_impl<std::numeric_limits<uint32_t>::max()>(
      block, dom);
}

size_t chain_and_dom_violations_coldstart(
    Block* block,
    const dominators::SimpleFastDominators<cfg::GraphInterface>& dom) {
  return chain_and_dom_violations_impl<1>(block, dom);
}

// Ugly but necessary for constexpr below.
using CounterFnPtr = size_t (*)(
    Block*, const dominators::SimpleFastDominators<cfg::GraphInterface>&);

constexpr std::array<std::pair<std::string_view, CounterFnPtr>, 11> gCounters =
    {{{"~blocks~count", &count_blocks},
      {"~blocks~with~source~blocks", &count_block_has_sbs},
      {"~blocks~with~incomplete-source~blocks",
       &count_block_has_incomplete_sbs},
      {"~assessment~source~blocks~total", &count_all_sbs},
      {"~flow~violation~in~chain", &chain_hot_violations},
      {"~flow~violation~in~chain~one", &chain_hot_one_violations},
      {"~flow~violation~chain~and~dom", &chain_and_dom_violations},
      {"~flow~violation~chain~and~dom.cold_start",
       &chain_and_dom_violations_coldstart},
      {"~flow~violation~seen~method~cold~entry",
       &hot_method_cold_entry_violations},
      {"~flow~violation~seen~method~cold~entry~blocks",
       &hot_method_cold_entry_block_violations},
      {"~flow~violation~hot~all~children~cold",
       &hot_all_children_cold_violations}}};

constexpr std::array<std::pair<std::string_view, CounterFnPtr>, 2>
    gCountersNonEntry = {{
        {"~flow~violation~idom", &hot_immediate_dom_not_hot},
        {"~flow~violation~direct~predecessors", &hot_no_hot_pred},
    }};

struct SourceBlocksStats {
  size_t methods_with_sbs{0};

  struct Data {
    size_t count{0};
    size_t methods{0};
    size_t min{std::numeric_limits<size_t>::max()};
    size_t max{0};
    struct Method {
      const DexMethod* method;
      size_t num_opcodes;
    };
    std::optional<Method> min_method;
    std::optional<Method> max_method;

    Data& operator+=(const Data& rhs) {
      count += rhs.count;
      methods += rhs.methods;

      min = std::min(min, rhs.min);
      max = std::max(max, rhs.max);

      auto set_min_max = [](auto& lhs, auto& rhs, auto fn) {
        if (!rhs) {
          return;
        }
        if (!lhs) {
          lhs = rhs;
          return;
        }
        auto op = fn(lhs->num_opcodes, rhs->num_opcodes);
        if (op == rhs->num_opcodes &&
            (op != lhs->num_opcodes ||
             compare_dexmethods(rhs->method, lhs->method))) {
          lhs = rhs;
        }
      };

      set_min_max(min_method, rhs.min_method,
                  [](auto lhs, auto rhs) { return std::min(lhs, rhs); });
      set_min_max(max_method, rhs.max_method,
                  [](auto lhs, auto rhs) { return std::max(lhs, rhs); });

      return *this;
    }

    void fill_derived(const DexMethod* m) {
      methods = count > 0 ? 1 : 0;
      min = max = count;

      if (count != 0) {
        size_t num_opcodes = m->get_code()->count_opcodes();
        min_method = max_method = (Method){m, num_opcodes};
      }
    }
  };

  std::array<Data, gCounters.size()> global{};
  std::array<Data, gCountersNonEntry.size()> non_entry{};

  SourceBlocksStats& operator+=(const SourceBlocksStats& that) {
    methods_with_sbs += that.methods_with_sbs;

    for (size_t i = 0; i != global.size(); ++i) {
      global[i] += that.global[i];
    }

    for (size_t i = 0; i != non_entry.size(); ++i) {
      non_entry[i] += that.non_entry[i];
    }

    return *this;
  }

  void fill_derived(const DexMethod* m) {
    static_assert(gCounters[1].first == "~blocks~with~source~blocks");
    methods_with_sbs = global[1].count > 0 ? 1 : 0;

    for (auto& data : global) {
      data.fill_derived(m);
    }

    for (auto& data : non_entry) {
      data.fill_derived(m);
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
        auto* code = m->get_code();
        if (code == nullptr) {
          return ret;
        }
        code->build_cfg();
        auto& cfg = code->cfg();
        auto dominators =
            dominators::SimpleFastDominators<cfg::GraphInterface>(cfg);

        for (auto* block : cfg.blocks()) {
          for (size_t i = 0; i != gCounters.size(); ++i) {
            ret.global[i].count += (*gCounters[i].second)(block, dominators);
          }
          if (block != cfg.entry_block()) {
            for (size_t i = 0; i != gCountersNonEntry.size(); ++i) {
              ret.non_entry[i].count +=
                  (*gCountersNonEntry[i].second)(block, dominators);
            }
          }
        }

        code->clear_cfg();

        ret.fill_derived(m);

        return ret;
      });

  size_t consistency_check_violations =
      s_sbcc.is_initialized() ? s_sbcc.run(build_class_scope(stores)) : 0;

  sm.set_metric("~consistency~check~violations", consistency_check_violations);

  sm.set_metric("~assessment~methods~with~sbs", stats.methods_with_sbs);

  auto data_metric = [&sm](const std::string_view& name, const auto& data) {
    sm.set_metric(name, data.count);

    auto scope = sm.scope(std::string(name));
    sm.set_metric("methods", data.methods);
    sm.set_metric("min", data.min);
    sm.set_metric("max", data.max);

    auto min_max = [&](const auto& m, const char* name) {
      if (m) {
        auto min_max_scope = sm.scope(name);
        sm.set_metric(show_deobfuscated(m->method), m->num_opcodes);
      }
    };
    min_max(data.min_method, "min_method");
    min_max(data.max_method, "max_method");
  };
  for (size_t i = 0; i != gCounters.size(); ++i) {
    data_metric(gCounters[i].first, stats.global[i]);
  }
  for (size_t i = 0; i != gCountersNonEntry.size(); ++i) {
    data_metric(gCountersNonEntry[i].first, stats.non_entry[i]);
  }
}

size_t compute_method_violations(const call_graph::Graph& call_graph,
                                 const Scope& scope) {
  size_t count{0};

  SourceBlockInvokeMap src_block_invoke_map;
  // Builds a graph of method -> invoke insn -> vector<bool> each bool
  // representing if the coldstart interaction of the source block right
  // before that invoke is hot or not
  walk::parallel::methods(scope, [&](DexMethod* current_method) {
    if (current_method == nullptr || current_method->get_code() == nullptr) {
      return;
    }

    ScopedCFG cfg(current_method->get_code());
    cfg->remove_unreachable_blocks();

    IRInstruction* current_invoke_insn = nullptr;
    for (auto* block : cfg->blocks()) {
      for (auto it = block->rbegin(); it != block->rend(); ++it) {
        if (it->type == MFLOW_OPCODE) {
          auto* instruction = it->insn;
          if (opcode::is_an_invoke(instruction->opcode())) {
            current_invoke_insn = instruction;
          }
        } else if (it->type == MFLOW_SOURCE_BLOCK) {
          if (current_invoke_insn != nullptr) {
            SourceBlock* sb = it->src_block.get();
            if ((sb != nullptr) && sb->vals_size == 0) {
              continue;
            }
            bool is_hot = (sb != nullptr) && sb->get_val(0).value_or(0) > 0;
            src_block_invoke_map.update(
                current_method, [&](const DexMethod*, auto& method_map, bool) {
                  method_map[current_invoke_insn].push_back(is_hot);
                });
            current_invoke_insn = nullptr;
          }
        }
      }
    }
  });

  impl::visit_by_levels(&call_graph, [&](call_graph::NodeId node) {
    count += hot_callee_all_cold_callers(node, src_block_invoke_map);
  });

  return count;
}

struct ViolationsHelper::ViolationsHelperImpl {
  size_t top_n;
  UnorderedMap<DexMethod*, size_t> violations_start;
  size_t method_violations{0};
  std::vector<std::string> print;
  Scope scope;
  bool processed{false};
  bool track_intermethod_violations{false};
  bool print_all_violations{false};

  using Violation = ViolationsHelper::Violation;
  const Violation v;

  struct MethodDelta {
    DexMethod* method;
    size_t violations_delta;
    size_t method_size;

    MethodDelta(DexMethod* p1, size_t p2, size_t p3)
        : method(p1), violations_delta(p2), method_size(p3) {}

    // Comparison operator for sorting by proportional violations (descending)
    bool operator<(const MethodDelta& other) const {
      double this_proportional_violations =
          (double)violations_delta / (double)method_size;
      double other_proportional_violations =
          (double)other.violations_delta / (double)other.method_size;
      if (this_proportional_violations > other_proportional_violations) {
        return true;
      }
      if (this_proportional_violations < other_proportional_violations) {
        return false;
      }

      if (violations_delta > other.violations_delta) {
        return true;
      }
      if (violations_delta < other.violations_delta) {
        return false;
      }

      if (method_size < other.method_size) {
        return true;
      }
      if (method_size > other.method_size) {
        return false;
      }

      return compare_dexmethods(method, other.method);
    }
  };

  ViolationsHelperImpl(Violation v,
                       const Scope& scope,
                       size_t top_n,
                       std::vector<std::string> to_vis,
                       bool track_intermethod_violations,
                       bool print_all_violations)
      : top_n(top_n),
        print(std::move(to_vis)),
        scope(scope),
        track_intermethod_violations(track_intermethod_violations),
        print_all_violations(print_all_violations),
        v(v) {
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

    if (track_intermethod_violations) {
      auto method_override_graph = method_override_graph::build_graph(scope);
      auto call_graph = std::make_unique<call_graph::Graph>(
          call_graph::single_callee_graph(*method_override_graph, scope));

      auto val = compute_method_violations(*call_graph, scope);
      method_violations = val;
    }

    print_all();
  }

  static size_t compute(Violation v, cfg::ControlFlowGraph& cfg) {
    switch (v) {
    case Violation::kHotImmediateDomNotHot:
      return hot_immediate_dom_not_hot_cfg(cfg);
    case Violation::kChainAndDom:
      return chain_and_dom_violations_cfg(cfg);
    case Violation::kUncoveredSourceBlocks:
      return uncovered_source_blocks_violations_cfg(cfg);
    case Violation::kHotMethodColdEntry:
      return hot_method_cold_entry_violations_cfg(cfg);
    case Violation::kHotNoHotPred:
      return hot_no_hot_pred_cfg(cfg);
    case Violation::KHotAllChildrenCold:
      return hot_all_children_cold_cfg(cfg);
    case Violation::ViolationSize:
      not_reached();
    }
    not_reached();
  }

  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~ViolationsHelperImpl() { process(nullptr); }

  void silence() { processed = true; }
  void process(ScopedMetrics* sm) {
    if (processed) {
      return;
    }
    processed = true;

    std::atomic<size_t> change_sum{0};
    long long method_violation_change_sum{0};

    {
      std::mutex lock;
      std::vector<MethodDelta> top_changes;

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
            if (top_changes.size() < top_n) {
              top_changes.emplace_back(m, m_delta, s);
              return;
            }
            MethodDelta m_t{m, m_delta, s};
            if (m_t < top_changes.back()) {
              top_changes.back() = m_t;
              std::sort(top_changes.begin(), top_changes.end());
            }
          },
          violations_start);

      struct MaybeMetrics {
        ScopedMetrics* root{nullptr};
        std::optional<ScopedMetrics::Scope> scope;
        explicit MaybeMetrics(ScopedMetrics* root) : root(root) {}
        explicit MaybeMetrics(ScopedMetrics* root, ScopedMetrics::Scope sc)
            : root(root), scope(std::move(sc)) {}
        void set_metric(const std::string_view& key, int64_t value) {
          if (root != nullptr) {
            root->set_metric(key, value);
          }
        }
        MaybeMetrics sub_scope(std::string key) {
          if (root == nullptr) {
            return MaybeMetrics(nullptr);
          }
          return MaybeMetrics(root, root->scope(std::move(key)));
        }
      };
      MaybeMetrics mm(sm);
      auto mm_top_changes = mm.sub_scope("top_changes");
      for (size_t i = 0; i != top_changes.size(); ++i) {
        auto& t = top_changes[i];
        TRACE(MMINL, 0, "%s (size %zu): +%zu", SHOW(t.method), t.method_size,
              t.violations_delta);
        auto mm_top_changes_i = mm_top_changes.sub_scope(std::to_string(i));
        {
          auto mm_top_changes_i_size = mm_top_changes_i.sub_scope("size");
          mm_top_changes_i_size.set_metric(show(t.method),
                                           static_cast<int64_t>(t.method_size));
        }
        {
          auto mm_top_changes_i_size = mm_top_changes_i.sub_scope("delta");
          mm_top_changes_i_size.set_metric(
              show(t.method), static_cast<int64_t>(t.violations_delta));
        }
      }
    }

    print_all();

    TRACE(MMINL, 0, "Introduced %zu violations.", change_sum.load());
    if (track_intermethod_violations) {
      auto method_override_graph = method_override_graph::build_graph(scope);
      auto call_graph = std::make_unique<call_graph::Graph>(
          call_graph::single_callee_graph(*method_override_graph, scope));
      auto val = compute_method_violations(*call_graph, scope);
      method_violation_change_sum =
          static_cast<long long>(val - method_violations);
      TRACE(MMINL, 0, "Introduced %lld inter-method violations.",
            method_violation_change_sum);
    }
    if (sm != nullptr) {
      sm->set_metric("new_violations", change_sum.load());
      sm->set_metric("new_method_violations", method_violation_change_sum);
    }
  }

  static size_t hot_immediate_dom_not_hot_cfg(cfg::ControlFlowGraph& cfg) {
    size_t sum{0};

    // Some passes may leave around unreachable blocks which the fast-dom
    // does not deal well with.
    cfg.remove_unreachable_blocks();
    dominators::SimpleFastDominators<cfg::GraphInterface> dom{cfg};

    for (auto* b : cfg.blocks()) {
      sum += hot_immediate_dom_not_hot(b, dom);
    }
    return sum;
  }

  static size_t chain_and_dom_violations_cfg(cfg::ControlFlowGraph& cfg) {
    size_t sum{0};

    // Some passes may leave around unreachable blocks which the fast-dom
    // does not deal well with.
    cfg.remove_unreachable_blocks();
    dominators::SimpleFastDominators<cfg::GraphInterface> dom{cfg};

    for (auto* b : cfg.blocks()) {
      sum += chain_and_dom_violations(b, dom);
    }
    return sum;
  }

  static size_t uncovered_source_blocks_violations_cfg(
      cfg::ControlFlowGraph& cfg) {
    size_t sum{0};
    for (auto* b : cfg.blocks()) {
      if (is_ghost_block(b)) {
        // Do not count ghost blocks in this violation
        continue;
      }
      auto* sb = get_first_source_block(b);
      if (sb == nullptr) {
        sum++;
      }
    }
    return sum;
  }

  static size_t hot_method_cold_entry_violations_cfg(
      cfg::ControlFlowGraph& cfg) {
    size_t sum{0};
    auto* entry_block = cfg.entry_block();
    if (entry_block == nullptr) {
      return 0;
    }
    auto* sb = get_first_source_block(entry_block);
    if (sb == nullptr) {
      return 0;
    }
    sb->foreach_val([&sum](const auto& val) {
      if (val && val->appear100 != 0 && val->val == 0) {
        sum++;
      }
    });
    return sum;
  }

  static size_t hot_no_hot_pred_cfg(cfg::ControlFlowGraph& cfg) {
    size_t sum{0};

    cfg.remove_unreachable_blocks();
    dominators::SimpleFastDominators<cfg::GraphInterface> dom{cfg};

    for (auto* b : cfg.blocks()) {
      sum += hot_no_hot_pred(b, dom);
    }
    return sum;
  }

  static size_t hot_all_children_cold_cfg(cfg::ControlFlowGraph& cfg) {
    size_t sum{0};

    cfg.remove_unreachable_blocks();

    for (auto* b : cfg.blocks()) {
      sum += hot_all_children_cold(b);
    }
    return sum;
  }

  void print_all() const {
    for (const auto& m_str : print) {
      auto* m = DexMethod::get_method(m_str);
      if (m != nullptr) {
        redex_assert(m != nullptr && m->is_def());
        auto* m_def = m->as_def();
        log_cfg_violations(v, m_def);
        if (print_all_violations) {
          print_cfg_with_all_violating_blocks(m_def);
        }
      }
    }

    walk::methods(scope, [this](DexMethod* m) {
      auto* code = m->get_code();
      if (code == nullptr) {
        return;
      }
      cfg::ScopedCFG cfg(code);
      const auto& cur_method_name = show(m);
      for (auto* block : cfg->blocks()) {
        auto vec = gather_source_blocks(block);
        for (auto* sb : vec) {
          auto it = std::find(print.begin(), print.end(), sb->src->str());
          if (it != std::end(print) && *it != cur_method_name) {
            TRACE(MMINL, 0, "### METHOD %s HAS SOURCE BLOCKS FROM %s ###",
                  cur_method_name.c_str(), (*it).c_str());
            log_cfg_violations(v, m->as_def());
            return;
          }
        }
      }
    });
  }

  template <typename SpecialT>
  static void print_cfg_with_violations(DexMethod* m) {
    cfg::ScopedCFG cfg(m->get_code());
    SpecialT special{*cfg};
    TRACE(MMINL, 0, "=== %s ===\n%s\n", SHOW(m),
          show<SpecialT>(*cfg, special).c_str());
  }

  // This map stores SourceBlock -> Violation Type -> Vector of Strings of
  // Block Names where the violation is happening. Nullptr entry stores all
  // uncovered source blocks.
  using ViolationBlockMap =
      UnorderedMap<SourceBlock*,
                   UnorderedMap<Violation, std::vector<std::string>>>;

  template <typename SpecialT>
  static void gather_cfg_violating_blocks(DexMethod* m,
                                          ViolationBlockMap* violating_blocks) {
    cfg::ScopedCFG cfg(m->get_code());
    SpecialT special{*cfg, violating_blocks};

    const auto& blocks = cfg->blocks();
    // This needs a no-op stringstream to pass into the functions below
    std::ostringstream ss;
    for (auto* b : blocks) {
      special.start_block(ss, b);
      for (const auto& mie : *b) {
        special.mie_before(ss, mie);
        special.mie_after(ss, mie);
      }
      special.end_block(ss, b);
    }
  }

  static std::string_view get_violation_name(Violation v) {
    switch (v) {
    case Violation::kChainAndDom: {
      return "ChainAndDom";
    }
    case Violation::kHotImmediateDomNotHot: {
      return "HotImmediateDomNotHot";
    }
    case Violation::KHotAllChildrenCold: {
      return "HotAllChildrenCold";
    }
    case Violation::kHotMethodColdEntry: {
      return "HotMethodColdEntry";
    }
    case Violation::kUncoveredSourceBlocks: {
      return "UncoveredSourceBlocks";
    }
    case Violation::kHotNoHotPred: {
      return "HotNoHotPred";
    }
    case Violation::ViolationSize: {
      not_reached();
    }
    }
    not_reached();
  }

  static void print_cfg_with_all_violating_blocks(DexMethod* m) {
    cfg::ScopedCFG cfg(m->get_code());
    ViolationBlockMap violation_blocks;
    for (Violation v = Violation::kHotImmediateDomNotHot;
         v < Violation::ViolationSize;
         v = static_cast<Violation>(static_cast<int>(v) + 1)) {
      // This is set so it does not actually print and clutter the output,
      // this is just used to call gather_cfg_violating_blocks

      log_cfg_violations(v, m, false, &violation_blocks);
    }

    std::stringstream ss;
    ss << "=== Violations ===\n";
    for (auto* block : cfg->blocks()) {
      auto source_block_vector = gather_source_blocks(block);
      bool block_has_violations = false;
      std::stringstream block_ss;
      block_ss << "B" << std::to_string(block->id()) << ":\n";
      for (auto* source_block : source_block_vector) {
        block_ss << " Source Block[" << source_block
                 << "]: " << source_block->src->c_str() << "@"
                 << source_block->id << ":\n";
        for (Violation v = Violation::kHotImmediateDomNotHot;
             v < Violation::ViolationSize;
             v = static_cast<Violation>(static_cast<int>(v) + 1)) {
          if (v != Violation::kUncoveredSourceBlocks) {
            block_ss << "  " << get_violation_name(v) << ": ";
            if (violation_blocks.count(source_block) != 0u) {
              block_has_violations = true;
              auto& violation_map = violation_blocks.at(source_block);
              if (violation_map.count(v) != 0u) {
                for (const auto& str : violation_map[v]) {
                  block_ss << str << ", ";
                }
              }
            }
            block_ss << '\n';
          }
        }
      }
      if (block_has_violations) {
        ss << block_ss.str() << '\n';
      }
    }
    // Add uncovered source blocks into the string
    if (violation_blocks.count(nullptr) != 0u) {
      auto& violation_map = violation_blocks.at(nullptr);
      Violation uncovered = Violation::kUncoveredSourceBlocks;
      if (violation_map.count(uncovered) != 0u) {
        ss << "  " << get_violation_name(uncovered) << ": ";
        for (const auto& str : violation_map[uncovered]) {
          ss << str << ", ";
        }
      }
    }
    TRACE(MMINL, 0, "%s\n", ss.str().c_str());
  }

  template <typename Derived>
  class ViolationVisitorBase {
   private:
    explicit ViolationVisitorBase(cfg::ControlFlowGraph& cfg)
        : violating_blocks(nullptr) {
      static_cast<Derived*>(this)->initialize(cfg);
    }

    explicit ViolationVisitorBase(cfg::ControlFlowGraph& cfg,
                                  ViolationBlockMap* violating_blocks)
        : violating_blocks(violating_blocks) {
      static_cast<Derived*>(this)->initialize(cfg);
    }

    friend Derived;

   protected:
    cfg::Block* cur{nullptr};
    ViolationBlockMap* violating_blocks{nullptr};

   public:
    void mie_before(std::ostream&, const MethodItemEntry&) {}

    void start_block(std::ostream& os, cfg::Block* b) {
      cur = b;
      static_cast<Derived*>(this)->on_start_block(os, b);
    }

    void end_block(std::ostream& os, cfg::Block* b) {
      static_cast<Derived*>(this)->on_end_block(os, b);
      cur = nullptr;
    }

    void mie_after(std::ostream& os, const MethodItemEntry& mie) {
      static_cast<Derived*>(this)->mie_after_impl(os, mie);
    }

   protected:
    void initialize(cfg::ControlFlowGraph&) {}
    void on_start_block(std::ostream&, cfg::Block*) {}
    void on_end_block(std::ostream&, cfg::Block*) {}
  };

  template <typename ViolationVisitor>
  static void handle_violation(DexMethod* m,
                               bool print_violations,
                               ViolationBlockMap* violation_blocks) {
    if (print_violations) {
      print_cfg_with_violations<ViolationVisitor>(m);
    }
    if (violation_blocks != nullptr) {
      gather_cfg_violating_blocks<ViolationVisitor>(m, violation_blocks);
    }
  }

  // This function can gather information on violating blocks (for all types
  // of violations) for each source block, as well as print out a
  // violation-labeled CFG (only for one violation at a time).
  static void log_cfg_violations(
      Violation v,
      DexMethod* m,
      bool print_violations = true,
      ViolationBlockMap* violation_blocks = nullptr) {
    switch (v) {
    case Violation::kHotImmediateDomNotHot: {
      struct HotImmediateSpecial : ViolationVisitorBase<HotImmediateSpecial> {
        dominators::SimpleFastDominators<cfg::GraphInterface> dom;

        using Base = ViolationVisitorBase<HotImmediateSpecial>;

        explicit HotImmediateSpecial(cfg::ControlFlowGraph& cfg)
            : Base(cfg), dom(cfg) {}

        explicit HotImmediateSpecial(cfg::ControlFlowGraph& cfg,
                                     ViolationBlockMap* violating_blocks)
            : Base(cfg, violating_blocks), dom(cfg) {}

        void mie_after_impl(std::ostream& os, const MethodItemEntry& mie) {
          if (mie.type != MFLOW_SOURCE_BLOCK) {
            return;
          }

          auto* immediate_dominator = dom.get_idom(cur);
          if (immediate_dominator == nullptr) {
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
          if (first_sb_immediate_dominator == nullptr) {
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

          if (violating_blocks != nullptr) {
            auto& map = (*violating_blocks)[mie.src_block.get()];
            map[Violation::kHotImmediateDomNotHot].emplace_back(
                "B" + std::to_string(immediate_dominator->id()));
          }
          os << " !!! B" << immediate_dominator->id() << ": ";
          auto* sb = first_sb_immediate_dominator;
          os << " \"" << show(sb->src) << "\"@" << sb->id;
          sb->foreach_val([&](const auto& val) {
            os << " ";
            if (val) {
              os << val->val << "/" << val->appear100;
            } else {
              os << "N/A";
            }
          });
          os << "\n";
        }
      };
      return handle_violation<HotImmediateSpecial>(m, print_violations,
                                                   violation_blocks);
    }
    case Violation::kChainAndDom: {
      struct ChainAndDom : ViolationVisitorBase<ChainAndDom> {
        ChainAndDomState state{};
        bool first_in_block{false};
        bool prev_insn_can_throw{false};
        dominators::SimpleFastDominators<cfg::GraphInterface> dom;

        using Base = ViolationVisitorBase<ChainAndDom>;

        explicit ChainAndDom(cfg::ControlFlowGraph& cfg)
            : Base(cfg), dom(cfg) {}

        explicit ChainAndDom(cfg::ControlFlowGraph& cfg,
                             ViolationBlockMap* violating_blocks)
            : Base(cfg, violating_blocks), dom(cfg) {}

        void start_block(std::ostream& os, cfg::Block* b) {
          Base::start_block(os, b);
          first_in_block = true;
        }

        void mie_after_impl(std::ostream& os, const MethodItemEntry& mie) {
          if (mie.type != MFLOW_SOURCE_BLOCK) {
            prev_insn_can_throw =
                prev_insn_can_throw || (mie.type == MFLOW_OPCODE &&
                                        opcode::can_throw(mie.insn->opcode()));
            return;
          }

          size_t old_count = state.violations;

          auto* sb = mie.src_block.get();

          chain_and_dom_update<std::numeric_limits<uint32_t>::max()>(
              cur, sb, first_in_block, prev_insn_can_throw, state, dom);

          const bool head_error = state.violations > old_count;
          const auto* dom_block = state.dom_block;

          first_in_block = false;

          for (auto* cur_sb = sb->next.get(); cur_sb != nullptr;
               cur_sb = cur_sb->next.get()) {
            chain_and_dom_update<std::numeric_limits<uint32_t>::max()>(
                cur, cur_sb, first_in_block, prev_insn_can_throw, state, dom);
          }

          prev_insn_can_throw = false;

          if (state.violations > old_count) {
            os << " !!!";
            if (head_error) {
              os << " HEAD";
              if (dom_block != nullptr) {
                if (violating_blocks != nullptr) {
                  auto& map = (*violating_blocks)[sb];
                  map[Violation::kChainAndDom].emplace_back(
                      "B" + std::to_string(dom_block->id()));
                }
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
      };
      return handle_violation<ChainAndDom>(m, print_violations,
                                           violation_blocks);
    }
    case Violation::kUncoveredSourceBlocks: {
      struct UncoveredSourceBlocks
          : ViolationVisitorBase<UncoveredSourceBlocks> {
        using Base = ViolationVisitorBase<UncoveredSourceBlocks>;

        explicit UncoveredSourceBlocks(cfg::ControlFlowGraph& cfg)
            : Base(cfg) {}

        explicit UncoveredSourceBlocks(cfg::ControlFlowGraph& cfg,
                                       ViolationBlockMap* violating_blocks)
            : Base(cfg, violating_blocks) {}

        void mie_after_impl(std::ostream&, const MethodItemEntry&) {}

        void start_block(std::ostream& os, cfg::Block* b) {
          // Don't call Base::start_block - custom logic only
          if (is_ghost_block(b)) {
            return;
          }

          if (get_first_source_block(b) == nullptr) {
            if (violating_blocks != nullptr) {
              auto& map = (*violating_blocks)[nullptr];
              map[Violation::kUncoveredSourceBlocks].emplace_back(
                  "B" + std::to_string(b->id()));
            }
            os << "!!!MISSING SOURCE BLOCK\n";
          }
        }

        void end_block(std::ostream& os, cfg::Block* b) {
          // Empty - don't call Base::end_block
        }
      };
      return handle_violation<UncoveredSourceBlocks>(m, print_violations,
                                                     violation_blocks);
    }
    case Violation::kHotMethodColdEntry: {
      struct HotMethodColdEntry : ViolationVisitorBase<HotMethodColdEntry> {
        bool is_entry_block{false};
        bool first_in_block{false};

        using Base = ViolationVisitorBase<HotMethodColdEntry>;

        explicit HotMethodColdEntry(cfg::ControlFlowGraph& cfg) : Base(cfg) {}

        explicit HotMethodColdEntry(cfg::ControlFlowGraph& cfg,
                                    ViolationBlockMap* violating_blocks)
            : Base(cfg, violating_blocks) {}

        void start_block(std::ostream& os, cfg::Block* b) {
          Base::start_block(os, b);
          is_entry_block = b->preds().empty();
          first_in_block = true;
        }

        void end_block(std::ostream& os, cfg::Block* b) {
          Base::end_block(os, b);
        }

        void mie_after_impl(std::ostream& os, const MethodItemEntry& mie) {
          if (mie.type != MFLOW_SOURCE_BLOCK || !is_entry_block ||
              !first_in_block) {
            return;
          }
          first_in_block = false;

          auto* sb = mie.src_block.get();
          bool violation_found_in_head{false};
          sb->foreach_val([&violation_found_in_head](const auto& val) {
            if (val && val->appear100 != 0 && val->val == 0) {
              violation_found_in_head = true;
            }
          });
          if (violation_found_in_head) {
            if (violating_blocks != nullptr) {
              auto& map = (*violating_blocks)[sb];
              map[Violation::kHotMethodColdEntry].emplace_back("HEAD");
            }
            os << " !!! HEAD SB: METHOD IS HOT BUT ENTRY IS COLD";
          }

          bool violation_found_in_chain{false};
          for (auto* cur_sb = sb->next.get(); cur_sb != nullptr;
               cur_sb = cur_sb->next.get()) {
            cur_sb->foreach_val([&violation_found_in_chain](const auto& val) {
              if (val && val->appear100 != 0 && val->val == 0) {
                violation_found_in_chain = true;
              }
            });
          }
          if (violation_found_in_chain) {
            if (violating_blocks != nullptr) {
              auto& map = (*violating_blocks)[sb];
              map[Violation::kHotMethodColdEntry].emplace_back("CHAIN");
            }
            os << " !!! CHAIN SB: METHOD IS HOT BUT ENTRY IS COLD";
          }
          os << "\n";
        }
      };
      return handle_violation<HotMethodColdEntry>(m, print_violations,
                                                  violation_blocks);
    }
    case Violation::kHotNoHotPred: {
      struct HotNoHotPred : ViolationVisitorBase<HotNoHotPred> {
        dominators::SimpleFastDominators<cfg::GraphInterface> dom;

        using Base = ViolationVisitorBase<HotNoHotPred>;

        explicit HotNoHotPred(cfg::ControlFlowGraph& cfg)
            : Base(cfg), dom(cfg) {}

        explicit HotNoHotPred(cfg::ControlFlowGraph& cfg,
                              ViolationBlockMap* violating_blocks)
            : Base(cfg, violating_blocks), dom(cfg) {}

        void mie_after_impl(std::ostream& os, const MethodItemEntry& mie) {
          if (mie.type != MFLOW_SOURCE_BLOCK) {
            return;
          }

          if (!source_blocks::has_source_block_positive_val(
                  mie.src_block.get())) {
            os << " NOT HOT\n";
            return;
          }

          bool violation_found = true;

          if (cur->preds().empty()) {
            violation_found = false;
          }

          std::vector<std::string> cold_preds;
          for (auto* predecessor : cur->preds()) {
            auto* first_sb_pred =
                source_blocks::get_first_source_block(predecessor->src());
            if (source_blocks::has_source_block_positive_val(first_sb_pred)) {
              violation_found = false;
              break;
            } else {
              if (predecessor->src() != nullptr &&
                  violating_blocks != nullptr) {
                cold_preds.emplace_back(
                    "B" + std::to_string(predecessor->src()->id()));
              }
            }
          }

          if (violation_found) {
            if (violating_blocks != nullptr) {
              auto& map = (*violating_blocks)[mie.src_block.get()];
              map[Violation::kHotNoHotPred] = cold_preds;
            }
            os << " !!! HOT BLOCK NO HOT PRED\n";
          }
        }
      };
      return handle_violation<HotNoHotPred>(m, print_violations,
                                            violation_blocks);
    }
    case Violation::KHotAllChildrenCold: {
      struct HotAllChildrenCold : ViolationVisitorBase<HotAllChildrenCold> {
        using Base = ViolationVisitorBase<HotAllChildrenCold>;

        explicit HotAllChildrenCold(cfg::ControlFlowGraph& cfg) : Base(cfg) {}

        explicit HotAllChildrenCold(cfg::ControlFlowGraph& cfg,
                                    ViolationBlockMap* violating_blocks)
            : Base(cfg, violating_blocks) {}

        void mie_after_impl(std::ostream& os, const MethodItemEntry& mie) {
          if (mie.type != MFLOW_SOURCE_BLOCK) {
            return;
          }

          auto* last_sb_before_throw =
              source_blocks::get_last_source_block_if_after_throw(cur);

          if (mie.src_block.get() != last_sb_before_throw) {
            return;
          }

          if ((last_sb_before_throw == nullptr) ||
              !has_source_block_positive_val(last_sb_before_throw)) {
            return;
          }

          os << " HOT\n";

          bool has_successor = false;
          std::vector<std::string> cold_succs;
          for (auto* successor : cur->succs()) {
            auto* first_sb_succ =
                source_blocks::get_first_source_block(successor->src());
            has_successor = true;
            if (has_source_block_positive_val(first_sb_succ)) {
              return;
            }
            if (violating_blocks != nullptr) {
              cold_succs.emplace_back("B" +
                                      std::to_string(successor->src()->id()));
            }
          }
          if (has_successor) {
            if (violating_blocks != nullptr) {
              auto& map = (*violating_blocks)[last_sb_before_throw];
              map[Violation::KHotAllChildrenCold] = cold_succs;
            }
            os << " !!! HOT ALL CHILDREN COLD\n";
          }
        }
      };
      return handle_violation<HotAllChildrenCold>(m, print_violations,
                                                  violation_blocks);
    }
    case Violation::ViolationSize:
      not_reached();
    }
    not_reached();
  }
};

ViolationsHelper::ViolationsHelper(Violation v,
                                   const Scope& scope,
                                   size_t top_n,
                                   std::vector<std::string> to_vis,
                                   bool track_intermethod_violations,
                                   bool print_all_violations)
    : impl(std::make_unique<ViolationsHelperImpl>(v,
                                                  scope,
                                                  top_n,
                                                  std::move(to_vis),
                                                  track_intermethod_violations,
                                                  print_all_violations)) {}
ViolationsHelper::~ViolationsHelper() {}

void ViolationsHelper::process(ScopedMetrics* sm) {
  if (impl) {
    impl->process(sm);
  }
}
void ViolationsHelper::silence() {
  if (impl) {
    impl->silence();
  }
}

ViolationsHelper::ViolationsHelper(ViolationsHelper&& other) noexcept {
  impl = std::move(other.impl);
}
ViolationsHelper& ViolationsHelper::operator=(ViolationsHelper&& rhs) noexcept {
  impl = std::move(rhs.impl);
  return *this;
}

SourceBlock* get_first_source_block_of_method(const DexMethod* m) {
  const auto* code = m->get_code();
  if (code->cfg_built()) {
    return get_first_source_block(code->cfg().entry_block());
  } else {
    for (const auto& mie : *code) {
      if (mie.type == MFLOW_SOURCE_BLOCK) {
        return mie.src_block.get();
      }
    };
  }
  return nullptr;
}

SourceBlock* get_any_first_source_block_of_methods(
    const std::vector<const DexMethod*>& methods) {
  for (const auto* m : methods) {
    auto* sb = get_first_source_block_of_method(m);
    if (sb != nullptr) {
      return sb;
    }
  }
  return nullptr;
}

void insert_synthetic_source_blocks_in_method(
    DexMethod* method,
    const std::function<std::unique_ptr<SourceBlock>()>& source_block_creator) {
  auto* code = method->get_code();
  cfg::ScopedCFG cfg(code);

  for (auto* block : cfg->blocks()) {
    if (block == cfg->entry_block()) {
      // Special handling.
      continue;
    }
    auto new_sb = source_block_creator();
    auto it = block->get_first_insn();
    if (it != block->end() && opcode::is_move_result_any(it->insn->opcode())) {
      block->insert_after(it, std::move(new_sb));
    } else {
      block->insert_before(it, std::move(new_sb));
    }
  }

  auto* block = cfg->entry_block();
  auto new_sb = source_block_creator();
  auto it = block->get_first_non_param_loading_insn();
  block->insert_before(it, std::move(new_sb));
}

std::unique_ptr<SourceBlock> clone_as_synthetic(SourceBlock* sb,
                                                const DexMethod* ref,
                                                const SourceBlock::Val& val) {
  std::unique_ptr<SourceBlock> new_sb = std::make_unique<SourceBlock>(*sb);
  new_sb->next.reset();
  new_sb->id = SourceBlock::kSyntheticId;
  if (ref != nullptr) {
    new_sb->src = ref->get_deobfuscated_name_or_null();
  }
  new_sb->fill(val);
  return new_sb;
}

std::unique_ptr<SourceBlock> clone_as_synthetic(
    SourceBlock* sb,
    const DexMethod* ref,
    const std::optional<SourceBlock::Val>& opt_val) {
  std::unique_ptr<SourceBlock> new_sb = std::make_unique<SourceBlock>(*sb);
  new_sb->next.reset();
  new_sb->id = SourceBlock::kSyntheticId;
  if (ref != nullptr) {
    new_sb->src = ref->get_deobfuscated_name_or_null();
  }
  if (opt_val) {
    new_sb->fill(*opt_val);
  }
  return new_sb;
}

std::unique_ptr<SourceBlock> clone_as_synthetic(
    SourceBlock* sb,
    const DexMethod* ref,
    const std::vector<SourceBlock*>& many) {
  std::unique_ptr<SourceBlock> new_sb = std::make_unique<SourceBlock>(*sb);
  new_sb->next.reset();
  new_sb->id = SourceBlock::kSyntheticId;
  if (ref != nullptr) {
    new_sb->src = ref->get_deobfuscated_name_or_null();
  }
  new_sb->fill(SourceBlock::Val::none());
  for (const auto& other : many) {
    new_sb->max(*other);
  }
  return new_sb;
}

void adjust_block_hits_with_appear100_threshold(
    ControlFlowGraph* cfg, int32_t block_appear100_threshold) {
  for (auto* block : cfg->blocks()) {
    for (auto& mie : *block) {
      if (mie.type == MFLOW_SOURCE_BLOCK) {
        for (auto* sb = mie.src_block.get(); sb != nullptr;
             sb = sb->next.get()) {
          sb->foreach_val([&](auto& val) {
            if (val && val->appear100 < block_appear100_threshold) {
              val->val = 0;
            }
          });
        }
      }
    }
  }
}

} // namespace source_blocks
