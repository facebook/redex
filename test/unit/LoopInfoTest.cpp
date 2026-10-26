/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <sparta/WeakTopologicalOrdering.h>

#include "IRAssembler.h"
#include "IRCode.h"
#include "LoopInfo.h"
#include "RedexTest.h"

class LoopInfoTest : public RedexTest {};

namespace {
// A method with a single self-loop: the block with `if-eqz` is a loop header
// with a back-edge to itself and an external predecessor (the entry block).
std::unique_ptr<IRCode> loop_code() {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (:loop)
      (if-eqz v0 :loop)
      (return-void)
    )
  )");
  code->build_cfg();
  return code;
}
} // namespace

// Passing a non-const cfg now binds the analysis-only constructor (the mutating
// path moved to make_with_preheaders), so no preheader block is inserted. This
// is the behavior the existing callers rely on.
TEST_F(LoopInfoTest, nonConstCfgBindsReadOnlyConstructor) {
  auto code = loop_code();
  auto& cfg = code->cfg();
  size_t blocks_before = cfg.num_blocks();

  loop_impl::LoopInfo info(cfg);

  EXPECT_EQ(info.num_loops(), 1);
  EXPECT_EQ(cfg.num_blocks(), blocks_before);
}

// make_with_preheaders inserts a dedicated preheader block before the loop
// header.
TEST_F(LoopInfoTest, withPreheadersInsertsPreheader) {
  auto code = loop_code();
  auto& cfg = code->cfg();
  size_t blocks_before = cfg.num_blocks();

  auto info = loop_impl::LoopInfo::make_with_preheaders(cfg);

  EXPECT_EQ(info.num_loops(), 1);
  EXPECT_EQ(cfg.num_blocks(), blocks_before + 1);
}

namespace {
// `for (v<reg> = 0; v<reg> < v15; ++v<reg>) { body }`
std::string counted_loop(const std::string& label,
                         int reg,
                         const std::string& body) {
  const std::string v = "v" + std::to_string(reg);
  return "(const " + v + " 0)(:H" + label + ")(if-ge " + v + " v15 :X" + label +
         ")" + body + "(add-int/lit " + v + " " + v + " 1)(goto :H" + label +
         ")(:X" + label + ")";
}
} // namespace

// A root loop holding two loops that each hold two loops. Each of the four
// innermost loops must have its own depth-2 loop as parent, never the root,
// whatever order the analysis visits loops in. Many copies give that order
// every chance to vary; run with `-c redex.perturb_unordered=true` as well.
TEST_F(LoopInfoTest, nestedLoopsHaveTheirImmediateParent) {
  const std::string a = counted_loop(
      "A", 1, counted_loop("a1", 2, "") + counted_loop("a2", 2, ""));
  const std::string b = counted_loop(
      "B", 1, counted_loop("b1", 2, "") + counted_loop("b2", 2, ""));
  const std::string src =
      "((load-param v15)" + counted_loop("R", 0, a + b) + "(return-void))";
  for (int copy = 0; copy < 200; ++copy) {
    auto code = assembler::ircode_from_string(src);
    code->build_cfg();
    loop_impl::LoopInfo info(code->cfg());
    ASSERT_EQ(info.num_loops(), 7);
    std::map<int, int> per_depth;
    for (auto& loop : info) {
      ++per_depth[loop.get_loop_depth()];
      auto* parent = loop.get_parent_loop();
      if (parent != nullptr) {
        EXPECT_EQ(parent->get_loop_depth(), loop.get_loop_depth() - 1);
        EXPECT_TRUE(parent->contains(loop.get_header()));
      }
      EXPECT_EQ(info.get_loop_for(loop.get_header()), &loop);
    }
    EXPECT_EQ(per_depth, (std::map<int, int>{{1, 1}, {2, 2}, {3, 4}}))
        << "copy " << copy;
  }
}

// A chain of 28 loops, each nested in the previous one: every level is
// reported, and each loop's parent is the next larger loop.
TEST_F(LoopInfoTest, deepNestReportsEveryLevel) {
  constexpr int kDepth = 28;
  std::string body;
  for (int d = kDepth - 1; d >= 0; --d) {
    body = counted_loop("d" + std::to_string(d), d % 15, body);
  }
  auto code = assembler::ircode_from_string("((load-param v15)" + body +
                                            "(return-void))");
  code->build_cfg();
  loop_impl::LoopInfo info(code->cfg());

  std::vector<loop_impl::Loop*> outermost_first;
  for (auto& loop : info) {
    outermost_first.push_back(&loop);
  }
  ASSERT_EQ(outermost_first.size(), kDepth);
  std::sort(outermost_first.begin(), outermost_first.end(),
            [](auto* a, auto* b) {
              return a->get_blocks().size() > b->get_blocks().size();
            });
  for (int i = 0; i < kDepth; ++i) {
    EXPECT_EQ(outermost_first[i]->get_loop_depth(), i + 1);
    EXPECT_EQ(outermost_first[i]->get_parent_loop(),
              i == 0 ? nullptr : outermost_first[i - 1]);
  }
}

class SimpleGraph final {
 public:
  SimpleGraph() {}

  void add_edge(const std::string& source, const std::string& target) {
    m_edges[source].insert(target);
  }

  std::vector<std::string> successors(const std::string& node) {
    auto& succs = m_edges[node];
    return std::vector<std::string>(succs.begin(), succs.end());
  }

 private:
  std::unordered_map<std::string, std::set<std::string>> m_edges;
  friend std::ostream& operator<<(std::ostream&, const SimpleGraph&);
};

TEST_F(LoopInfoTest, visit_depth_first) {
  // 1 2 (3 4 (5 6) 7) 8
  SimpleGraph g;
  g.add_edge("1", "2");
  g.add_edge("2", "3");
  g.add_edge("3", "4");
  g.add_edge("4", "5");
  g.add_edge("5", "6");
  g.add_edge("6", "7");
  g.add_edge("7", "8");
  g.add_edge("2", "8");
  g.add_edge("4", "7");
  g.add_edge("6", "5");
  g.add_edge("7", "3");

  sparta::WeakTopologicalOrdering<std::string> wto(
      "1", [&g](const std::string& n) { return g.successors(n); });

  std::ostringstream s;
  for (const auto& comp : wto) {
    loop_impl::visit_depth_first<std::string>(
        comp, [&s](const std::string& v) { s << v; });
    s << "\n";
  }

  std::ostringstream check;
  check << 1 << "\n" << 2 << "\n" << 34567 << "\n" << 8 << "\n";
  EXPECT_EQ(s.str(), check.str());
}

TEST_F(LoopInfoTest, construct_level_order_traversal) {
  // 1 2 (3 4 (5 6) 7) 8 (9)
  SimpleGraph g;
  g.add_edge("1", "2");
  g.add_edge("2", "3");
  g.add_edge("3", "4");
  g.add_edge("4", "5");
  g.add_edge("5", "6");
  g.add_edge("6", "7");
  g.add_edge("7", "8");
  g.add_edge("2", "8");
  g.add_edge("4", "7");
  g.add_edge("6", "5");
  g.add_edge("7", "3");
  g.add_edge("8", "9");
  g.add_edge("9", "9");

  sparta::WeakTopologicalOrdering<std::string> wto(
      "1", [&g](const std::string& n) { return g.successors(n); });

  std::vector<std::reference_wrapper<const sparta::WtoComponent<std::string>>>
      level_order;

  loop_impl::construct_level_order_traversal(level_order, wto);

  EXPECT_EQ(level_order.at(0).get().head_node(), "3");
  EXPECT_EQ(level_order.at(1).get().head_node(), "9");
  EXPECT_EQ(level_order.at(2).get().head_node(), "5");
}
