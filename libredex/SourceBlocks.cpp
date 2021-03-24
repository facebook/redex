/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SourceBlocks.h"

#include <sstream>

#include "Debug.h"
#include "DexClass.h"
#include "S_Expression.h"
#include "Show.h"
#include "Trace.h"

namespace source_blocks {

using namespace cfg;
using namespace sparta;

namespace {

struct InsertHelper {
  DexMethod* method;
  uint32_t id{0};
  std::ostringstream oss;
  bool serialize;

  s_expr root_expr;
  std::vector<s_expr> expr_stack;
  bool had_profile_failure{false};

  InsertHelper(DexMethod* method, const std::string* profile, bool serialize)
      : method(method), serialize(serialize) {
    if (profile != nullptr) {
      std::istringstream iss{*profile};
      s_expr_istream s_expr_input(iss);
      s_expr_input >> root_expr;
      always_assert_log(!s_expr_input.fail(),
                        "Failed parsing profile %s for %s: %s",
                        profile->c_str(),
                        SHOW(method),
                        s_expr_input.what().c_str());
      expr_stack.push_back(s_expr({root_expr}));
    }
  }

  void start(Block* cur) {
    if (serialize) {
      oss << "(" << id;
    }

    float val = start_profile(cur);

    source_blocks::impl::BlockAccessor::push_source_block(
        cur, std::make_unique<SourceBlock>(method, id, val));
    ++id;
  }

  float start_profile(Block* cur) {
    if (had_profile_failure) {
      return 0.0f;
    }
    if (root_expr.is_nil()) {
      return 0.0f;
    }
    if (expr_stack.empty()) {
      had_profile_failure = true;
      TRACE(MMINL, 3,
            "Failed profile matching for %s: missing element for block %zu",
            SHOW(method), cur->id());
      return 0.0f;
    }
    std::string val_str;
    const s_expr& e = expr_stack.back();
    s_expr tail, inner_tail;
    if (!s_patn({s_patn({s_patn(&val_str)}, inner_tail)}, tail).match_with(e)) {
      had_profile_failure = true;
      TRACE(MMINL, 3,
            "Failed profile matching for %s: cannot match string for %s",
            SHOW(method), e.str().c_str());
      return 0.0f;
    }
    size_t after_idx;
    float val = std::stof(val_str, &after_idx); // May throw.
    always_assert_log(after_idx == val_str.size(),
                      "Could not parse %s as float",
                      val_str.c_str());
    TRACE(MMINL,
          5,
          "Started block with val=%f. Popping %s, pushing %s + %s",
          val,
          e.str().c_str(),
          tail.str().c_str(),
          inner_tail.str().c_str());
    expr_stack.pop_back();
    expr_stack.push_back(tail);
    expr_stack.push_back(inner_tail);
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

  void edge_profile(Block* /*cur*/, const Edge* e) {
    // If running with profile, there should be at least a nil on.
    if (had_profile_failure || expr_stack.empty()) {
      return;
    }
    std::string val;
    s_expr& expr = expr_stack.back();
    s_expr tail;
    if (!s_patn({s_patn(&val)}, tail).match_with(expr)) {
      had_profile_failure = true;
      TRACE(MMINL, 3,
            "Failed profile matching for %s: cannot match string for %s",
            SHOW(method), expr.str().c_str());
      return;
    }
    std::string expected(1, get_edge_char(e));
    if (expected != val) {
      had_profile_failure = true;
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
    expr_stack.pop_back();
    expr_stack.push_back(tail);
  }

  void end(Block* cur) {
    if (serialize) {
      oss << ")";
    }
    end_profile(cur);
  }

  void end_profile(Block* /*cur*/) {
    if (had_profile_failure) {
      return;
    }
    if (root_expr.is_nil()) {
      return;
    }
    if (expr_stack.empty()) {
      TRACE(MMINL,
            3,
            "Failed profile matching for %s: empty stack on close",
            SHOW(method));
      had_profile_failure = true;
      return;
    }
    if (!expr_stack.back().is_nil()) {
      TRACE(MMINL,
            3,
            "Failed profile matching for %s: edge sentinel not NIL",
            SHOW(method));
      had_profile_failure = true;
      return;
    }
    TRACE(MMINL, 5, "Popping %s", expr_stack.back().str().c_str());
    expr_stack.pop_back(); // Remove sentinel nil.
  }
};

} // namespace

InsertResult insert_source_blocks(DexMethod* method,
                                  ControlFlowGraph* cfg,
                                  const std::string* profile,
                                  bool serialize) {
  InsertHelper helper(method, profile, serialize);

  impl::visit_in_order(
      cfg, [&](Block* cur) { helper.start(cur); },
      [&](Block* cur, const Edge* e) { helper.edge(cur, e); },
      [&](Block* cur) { helper.end(cur); });

  if (helper.had_profile_failure) {
    // Reset all values.
    for (auto* b : cfg->blocks()) {
      auto vec = gather_source_blocks(b);
      for (auto* sb : vec) {
        const_cast<SourceBlock*>(sb)->val = 0.0f;
      }
    }
  }

  return {cfg->blocks().size(), helper.oss.str(), !helper.had_profile_failure};
}

} // namespace source_blocks
