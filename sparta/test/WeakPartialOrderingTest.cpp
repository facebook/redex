/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WeakPartialOrdering.h"

#include <gtest/gtest.h>
#include <set>
#include <sstream>
#include <sstream>
#include <stack>
#include <string>
#include <unordered_map>

using namespace sparta;

using WpoIdx = uint32_t;

class SimpleGraph2 final {
 public:
  SimpleGraph2() {}

  void add_edge(const std::string& source, const std::string& target) {
    m_edges[source].insert(target);
  }

  std::vector<std::string> successors(const std::string& node) {
    auto& succs = m_edges[node];
    return std::vector<std::string>(succs.begin(), succs.end());
  }

 private:
  std::unordered_map<std::string, std::set<std::string>> m_edges;
  friend std::ostream& operator<<(std::ostream&, const SimpleGraph2&);
};

/*
 * Print the graph in the DOT graph description language. You can use Graphviz
 * or a similar program to render the output.
 */
std::ostream& operator<<(std::ostream& o, const SimpleGraph2& g) {
  o << "digraph {\n";
  for (auto& edge : g.m_edges) {
    for (auto& succ : edge.second) {
      o << edge.first << " -> " << succ << "\n";
    }
  }
  o << "}\n";
  return o;
}

struct Answer {
  std::string node;
  bool plain;
  bool head;
  bool exit;
  uint32_t num_succs;
  uint32_t num_preds;
  uint32_t num_outer_preds;
  bool widen;
};

/*
 * This graph and the corresponding weak partial ordering are described
 * on page 4 of Bourdoncle's paper:
 *   F. Bourdoncle. Efficient chaotic iteration strategies with widenings.
 *   In Formal Methods in Programming and Their Applications, pp 128-141.
 * The graph is given as follows:
 *
 *                 +-----------------------+
 *                 |           +-----+     |
 *                 |           |     |     |
 *                 V           V     |     |
 *     1 --> 2 --> 3 --> 4 --> 5 --> 6 --> 7 --> 8
 *           |           |                 ^     ^
 *           |           |                 |     |
 *           |           +-----------------+     |
 *           +-----------------------------------+
 *
 * Bourdoncle's algorithm computes the following weak partial ordering:
 *
 *     1 2 (3 4 (5 6) 7) 8
 */
TEST(WeakPartialOrderingTest, exampleFromWtoPaper) {
  SimpleGraph2 g;
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
  // "1 2 (3 4 (5 6) 7) 8"

  WeakPartialOrdering<std::string> wpo(
      "1", [&g](const std::string& n) { return g.successors(n); }, false);

  EXPECT_FALSE(wpo.is_from_outside("5", "6"));
  EXPECT_FALSE(wpo.is_from_outside("3", "7"));
  EXPECT_TRUE(wpo.is_from_outside("3", "2"));
  EXPECT_FALSE(wpo.is_from_outside("3", "4"));

  EXPECT_EQ(10, wpo.size());

  // node, plain, head, exit, num_succs, num_preds, num_outer_preds
  // Notice that forward edges are not considered.
  Answer lst[] = {
      {"1", true, false, false, 1, 0, 0, false},
      {"2", true, false, false, 1, 1, 0, false},
      {"3", false, true, false, 1, 1, 0, false},
      {"4", true, false, false, 1, 1, 0, false},
      {"5", false, true, false, 1, 1, 0, true},
      {"6", true, false, false, 1, 1, 0, false},
      {"5", false, false, true, 1, 1, 1, false},
      {"7", true, false, false, 1, 1, 0, false},
      {"3", false, false, true, 1, 1, 1, false},
      {"8", true, false, false, 0, 1, 0, false},
  };

  std::unordered_map<WpoIdx, uint32_t> count(wpo.size());
  std::stack<WpoIdx> wl;
  wl.push(wpo.get_entry());
  int i = 0;
  std::ostringstream wto;
  bool first = true;
  while (!wl.empty()) {
    auto v = wl.top();
    wl.pop();
    for (auto w : wpo.get_successors(v)) {
      count[w]++;
      if (count[w] == wpo.get_num_preds(w)) {
        wl.push(w);
      }
    }

    auto& answer = lst[i++];
    EXPECT_EQ(answer.node, wpo.get_node(v)) << wpo.get_node(v);
    EXPECT_EQ(answer.plain, wpo.is_plain(v)) << wpo.get_node(v);
    EXPECT_EQ(answer.head, wpo.is_head(v)) << wpo.get_node(v);
    EXPECT_EQ(answer.exit, wpo.is_exit(v)) << wpo.get_node(v);
    EXPECT_EQ(answer.num_succs, wpo.get_successors(v).size())
        << wpo.get_node(v);
    EXPECT_EQ(answer.num_preds, wpo.get_num_preds(v)) << wpo.get_node(v);
    EXPECT_EQ(answer.widen, wpo.is_widening_point(v)) << wpo.get_node(v);
    if (wpo.is_head(v)) {
      EXPECT_EQ(wpo.get_node(wpo.get_exit_of_head(v)), wpo.get_node(v))
          << wpo.get_node(v);
      if (!first) {
        wto << ' ';
      }
      wto << '(' << wpo.get_node(v);
    } else if (wpo.is_exit(v)) {
      EXPECT_EQ(answer.num_outer_preds, wpo.get_num_outer_preds(v).size())
          << wpo.get_node(v);
      EXPECT_EQ(wpo.get_node(wpo.get_head_of_exit(v)), wpo.get_node(v))
          << wpo.get_node(v);
      wto << ')';
    } else {
      if (!first) {
        wto << ' ';
      }
      wto << wpo.get_node(v);
    }
    if (first) {
      first = false;
    }
  }
  EXPECT_EQ(wto.str(), "1 2 (3 4 (5 6) 7) 8");
}

/*
 * Check that we correctly handle the edge cases where we have a single-node
 * SCC as the last element of the top-level list of components, or as the last
 * subcomponent in a component
 */
TEST(WeakPartialOrderingTest, SingletonSccAtEnd) {
  {
    //             +--+
    //             v  |
    // +---+     +------+
    // | 1 | --> |  2   |
    // +---+     +------+
    SimpleGraph2 g;
    g.add_edge("1", "2");
    g.add_edge("2", "2");
    WeakPartialOrdering<std::string> wpo(
        "1", [&g](const std::string& n) { return g.successors(n); }, false);
    // "1 (2)"

    EXPECT_FALSE(wpo.is_from_outside("2", "2"));
    EXPECT_TRUE(wpo.is_from_outside("2", "1"));
    EXPECT_TRUE(wpo.is_from_outside("2", "1"));

    EXPECT_EQ(3, wpo.size());

    // node, plain, head, exit, num_succs, num_preds, num_outer_preds
    Answer lst[] = {
        {"1", true, false, false, 1, 0, 0, false},
        {"2", false, true, false, 1, 1, 0, true},
        {"2", false, false, true, 0, 1, 1, false},
    };

    std::unordered_map<WpoIdx, uint32_t> count(wpo.size());
    std::stack<WpoIdx> wl;
    wl.push(wpo.get_entry());
    int i = 0;
    std::ostringstream wto;
    bool first = true;
    while (!wl.empty()) {
      auto v = wl.top();
      wl.pop();
      for (auto w : wpo.get_successors(v)) {
        count[w]++;
        if (count[w] == wpo.get_num_preds(w)) {
          wl.push(w);
        }
      }

      auto& answer = lst[i++];
      EXPECT_EQ(answer.node, wpo.get_node(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.plain, wpo.is_plain(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.head, wpo.is_head(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.exit, wpo.is_exit(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.num_succs, wpo.get_successors(v).size())
          << wpo.get_node(v);
      EXPECT_EQ(answer.num_preds, wpo.get_num_preds(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.widen, wpo.is_widening_point(v)) << wpo.get_node(v);
      if (wpo.is_head(v)) {
        EXPECT_EQ(wpo.get_node(wpo.get_exit_of_head(v)), wpo.get_node(v))
            << wpo.get_node(v);
        if (!first) {
          wto << ' ';
        }
        wto << '(' << wpo.get_node(v);
      } else if (wpo.is_exit(v)) {
        EXPECT_EQ(answer.num_outer_preds, wpo.get_num_outer_preds(v).size())
            << wpo.get_node(v);
        EXPECT_EQ(wpo.get_node(wpo.get_head_of_exit(v)), wpo.get_node(v))
            << wpo.get_node(v);
        wto << ')';
      } else {
        if (!first) {
          wto << ' ';
        }
        wto << wpo.get_node(v);
      }
      if (first) {
        first = false;
      }
    }
    EXPECT_EQ(wto.str(), "1 (2)");
  }

  {
    //             +--+
    //             v  |
    // +---+     +------+     +---+
    // | 1 | <-- |  2   | --> | 3 |
    // +---+     +------+     +---+
    //   |         ^
    //   +---------+
    SimpleGraph2 g;
    g.add_edge("1", "2");
    g.add_edge("2", "2");
    g.add_edge("2", "1");
    g.add_edge("2", "3");
    WeakPartialOrdering<std::string> wpo(
        "1", [&g](const std::string& n) { return g.successors(n); }, false);
    // "(1 (2)) 3"

    EXPECT_FALSE(wpo.is_from_outside("2", "2"));
    EXPECT_FALSE(wpo.is_from_outside("1", "2"));
    EXPECT_TRUE(wpo.is_from_outside("2", "1"));

    EXPECT_EQ(5, wpo.size());

    // node, plain, head, exit, num_succs, num_preds, num_outer_preds
    Answer lst[] = {
        {"1", false, true, false, 1, 0, 0, false},
        {"2", false, true, false, 1, 1, 0, true},
        {"2", false, false, true, 1, 1, 1, false},
        {"1", false, false, true, 1, 1, 0, false},
        {"3", true, false, false, 0, 1, 0, false},
    };

    std::unordered_map<WpoIdx, uint32_t> count(wpo.size());
    std::stack<WpoIdx> wl;
    wl.push(wpo.get_entry());
    int i = 0;
    std::ostringstream wto;
    bool first = true;
    while (!wl.empty()) {
      auto v = wl.top();
      wl.pop();
      for (auto w : wpo.get_successors(v)) {
        count[w]++;
        if (count[w] == wpo.get_num_preds(w)) {
          wl.push(w);
        }
      }

      auto& answer = lst[i++];
      EXPECT_EQ(answer.node, wpo.get_node(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.plain, wpo.is_plain(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.head, wpo.is_head(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.exit, wpo.is_exit(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.num_succs, wpo.get_successors(v).size())
          << wpo.get_node(v);
      EXPECT_EQ(answer.num_preds, wpo.get_num_preds(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.widen, wpo.is_widening_point(v)) << wpo.get_node(v);
      if (wpo.is_head(v)) {
        EXPECT_EQ(wpo.get_node(wpo.get_exit_of_head(v)), wpo.get_node(v))
            << wpo.get_node(v);
        if (!first) {
          wto << ' ';
        }
        wto << '(' << wpo.get_node(v);
      } else if (wpo.is_exit(v)) {
        EXPECT_EQ(answer.num_outer_preds, wpo.get_num_outer_preds(v).size())
            << wpo.get_node(v);
        EXPECT_EQ(wpo.get_node(wpo.get_head_of_exit(v)), wpo.get_node(v))
            << wpo.get_node(v);
        wto << ')';
      } else {
        if (!first) {
          wto << ' ';
        }
        wto << wpo.get_node(v);
      }
      if (first) {
        first = false;
      }
    }
    EXPECT_EQ(wto.str(), "(1 (2)) 3");
  }
}

/*
 * Check that we correctly handle the edge cases where we have a multi-node
 * SCC as the last element of the top-level list of components, or as the last
 * subcomponent in a component
 */
TEST(WeakPartialOrderingTest, SccAtEnd) {
  {
    //             +---------+
    //             v         |
    // +---+     +---+     +---+
    // | 1 | --> | 2 | --> | 3 |
    // +---+     +---+     +---+
    SimpleGraph2 g;
    g.add_edge("1", "2");
    g.add_edge("2", "3");
    g.add_edge("3", "2");

    WeakPartialOrdering<std::string> wpo(
        "1", [&g](const std::string& n) { return g.successors(n); }, false);
    // "1 (2 3)"

    EXPECT_FALSE(wpo.is_from_outside("2", "3"));
    EXPECT_TRUE(wpo.is_from_outside("2", "1"));

    EXPECT_EQ(4, wpo.size());

    // node, plain, head, exit, num_succs, num_preds, num_outer_preds
    Answer lst[] = {
        {"1", true, false, false, 1, 0, 0, false},
        {"2", false, true, false, 1, 1, 0, true},
        {"3", true, false, false, 1, 1, 0, false},
        {"2", false, false, true, 0, 1, 1, false},
    };

    std::unordered_map<WpoIdx, uint32_t> count(wpo.size());
    std::stack<WpoIdx> wl;
    wl.push(wpo.get_entry());
    int i = 0;
    std::ostringstream wto;
    bool first = true;
    while (!wl.empty()) {
      auto v = wl.top();
      wl.pop();
      for (auto w : wpo.get_successors(v)) {
        count[w]++;
        if (count[w] == wpo.get_num_preds(w)) {
          wl.push(w);
        }
      }

      auto& answer = lst[i++];
      EXPECT_EQ(answer.node, wpo.get_node(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.plain, wpo.is_plain(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.head, wpo.is_head(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.exit, wpo.is_exit(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.num_succs, wpo.get_successors(v).size())
          << wpo.get_node(v);
      EXPECT_EQ(answer.num_preds, wpo.get_num_preds(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.widen, wpo.is_widening_point(v)) << wpo.get_node(v);
      if (wpo.is_head(v)) {
        EXPECT_EQ(wpo.get_node(wpo.get_exit_of_head(v)), wpo.get_node(v))
            << wpo.get_node(v);
        if (!first) {
          wto << ' ';
        }
        wto << '(' << wpo.get_node(v);
      } else if (wpo.is_exit(v)) {
        EXPECT_EQ(answer.num_outer_preds, wpo.get_num_outer_preds(v).size())
            << wpo.get_node(v);
        EXPECT_EQ(wpo.get_node(wpo.get_head_of_exit(v)), wpo.get_node(v))
            << wpo.get_node(v);
        wto << ')';
      } else {
        if (!first) {
          wto << ' ';
        }
        wto << wpo.get_node(v);
      }
      if (first) {
        first = false;
      }
    }
    EXPECT_EQ(wto.str(), "1 (2 3)");
  }

  {
    //   +-------------------+
    //   |                   v
    // +---+     +---+     +---+     +---+
    // | 2 | <-- | 1 | <-- | 3 | --> | 4 |
    // +---+     +---+     +---+     +---+
    //   ^                   |
    //   +-------------------+
    SimpleGraph2 g;
    g.add_edge("1", "2");
    g.add_edge("2", "3");
    g.add_edge("3", "2");
    g.add_edge("3", "1");
    g.add_edge("3", "4");

    WeakPartialOrdering<std::string> wpo(
        "1", [&g](const std::string& n) { return g.successors(n); }, false);
    // "(1 (2 3)) 4"

    EXPECT_FALSE(wpo.is_from_outside("1", "3"));
    EXPECT_FALSE(wpo.is_from_outside("2", "3"));
    EXPECT_TRUE(wpo.is_from_outside("2", "1"));

    EXPECT_EQ(6, wpo.size());

    // node, plain, head, exit, num_succs, num_preds, num_outer_preds
    Answer lst[] = {
        {"1", false, true, false, 1, 0, 0, false},
        {"2", false, true, false, 1, 1, 0, true},
        {"3", true, false, false, 1, 1, 0, false},
        {"2", false, false, true, 1, 1, 1, false},
        {"1", false, false, true, 1, 1, 0, false},
        {"4", true, false, false, 0, 1, 0, false},
    };

    std::unordered_map<WpoIdx, uint32_t> count(wpo.size());
    std::stack<WpoIdx> wl;
    wl.push(wpo.get_entry());
    int i = 0;
    std::ostringstream wto;
    bool first = true;
    while (!wl.empty()) {
      auto v = wl.top();
      wl.pop();
      for (auto w : wpo.get_successors(v)) {
        count[w]++;
        if (count[w] == wpo.get_num_preds(w)) {
          wl.push(w);
        }
      }

      auto& answer = lst[i++];
      EXPECT_EQ(answer.node, wpo.get_node(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.plain, wpo.is_plain(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.head, wpo.is_head(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.exit, wpo.is_exit(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.num_succs, wpo.get_successors(v).size())
          << wpo.get_node(v);
      EXPECT_EQ(answer.num_preds, wpo.get_num_preds(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.widen, wpo.is_widening_point(v)) << wpo.get_node(v);
      if (wpo.is_head(v)) {
        EXPECT_EQ(wpo.get_node(wpo.get_exit_of_head(v)), wpo.get_node(v))
            << wpo.get_node(v);
        if (!first) {
          wto << ' ';
        }
        wto << '(' << wpo.get_node(v);
      } else if (wpo.is_exit(v)) {
        EXPECT_EQ(answer.num_outer_preds, wpo.get_num_outer_preds(v).size())
            << wpo.get_node(v);
        EXPECT_EQ(wpo.get_node(wpo.get_head_of_exit(v)), wpo.get_node(v))
            << wpo.get_node(v);
        wto << ')';
      } else {
        if (!first) {
          wto << ' ';
        }
        wto << wpo.get_node(v);
      }
      if (first) {
        first = false;
      }
    }
    EXPECT_EQ(wto.str(), "(1 (2 3)) 4");
  }
}

TEST(WeakPartialOrderingTest, SingleNode) {
  {
    // +---+
    // | 1 |
    // +---+
    SimpleGraph2 g;
    WeakPartialOrdering<std::string> wpo(
        "1", [&g](const std::string& n) { return g.successors(n); }, false);
    // "1"

    EXPECT_EQ(1, wpo.size());

    // node, plain, head, exit, num_succs, num_preds, num_outer_preds
    Answer lst[] = {
        {"1", true, false, false, 0, 0, 0, false},
    };

    std::unordered_map<WpoIdx, uint32_t> count(wpo.size());
    std::stack<WpoIdx> wl;
    wl.push(wpo.get_entry());
    int i = 0;
    std::ostringstream wto;
    bool first = true;
    while (!wl.empty()) {
      auto v = wl.top();
      wl.pop();
      for (auto w : wpo.get_successors(v)) {
        count[w]++;
        if (count[w] == wpo.get_num_preds(w)) {
          wl.push(w);
        }
      }

      auto& answer = lst[i++];
      EXPECT_EQ(answer.node, wpo.get_node(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.plain, wpo.is_plain(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.head, wpo.is_head(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.exit, wpo.is_exit(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.num_succs, wpo.get_successors(v).size())
          << wpo.get_node(v);
      EXPECT_EQ(answer.num_preds, wpo.get_num_preds(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.widen, wpo.is_widening_point(v)) << wpo.get_node(v);
      if (wpo.is_head(v)) {
        EXPECT_EQ(wpo.get_node(wpo.get_exit_of_head(v)), wpo.get_node(v))
            << wpo.get_node(v);
        if (!first) {
          wto << ' ';
        }
        wto << '(' << wpo.get_node(v);
      } else if (wpo.is_exit(v)) {
        EXPECT_EQ(answer.num_outer_preds, wpo.get_num_outer_preds(v).size())
            << wpo.get_node(v);
        EXPECT_EQ(wpo.get_node(wpo.get_head_of_exit(v)), wpo.get_node(v))
            << wpo.get_node(v);
        wto << ')';
      } else {
        if (!first) {
          wto << ' ';
        }
        wto << wpo.get_node(v);
      }
      if (first) {
        first = false;
      }
    }
    EXPECT_EQ(wto.str(), "1");
  }
}

TEST(WeakPartialOrderingTest, exampleFromWpoPaper) {
  {
    SimpleGraph2 g;
    g.add_edge("1", "2");
    g.add_edge("2", "3");
    g.add_edge("3", "4");
    g.add_edge("4", "3");
    g.add_edge("3", "5");
    g.add_edge("5", "2");
    g.add_edge("2", "6");
    g.add_edge("6", "5");
    g.add_edge("6", "7");
    g.add_edge("7", "8");
    g.add_edge("8", "6");
    g.add_edge("6", "9");
    g.add_edge("9", "8");
    g.add_edge("2", "10");

    WeakPartialOrdering<std::string> wpo(
        "1", [&g](const std::string& n) { return g.successors(n); }, false);
    // "1 (2 (3 4) (6 7 9 8) 5) 10"

    EXPECT_EQ(13, wpo.size());

    // node, plain, head, exit, num_succs, num_preds, num_outer_preds
    Answer lst[] = {
        {"1", true, false, false, 1, 0, 0, false},
        {"2", false, true, false, 2, 1, 0, false},
        {"3", false, true, false, 1, 1, 0, true},
        {"4", true, false, false, 1, 1, 0, false},
        {"3", false, false, true, 1, 1, 1, false},
        {"6", false, true, false, 2, 1, 0, true},
        {"7", true, false, false, 1, 1, 0, false},
        {"9", true, false, false, 1, 1, 0, false},
        {"8", true, false, false, 1, 2, 0, false},
        {"6", false, false, true, 1, 1, 1, false},
        {"5", true, false, false, 1, 2, 0, false},
        {"2", false, false, true, 1, 1, 1, false},
        {"10", true, false, false, 0, 1, 0, false},
    };

    std::unordered_map<WpoIdx, uint32_t> count(wpo.size());
    std::stack<WpoIdx> wl;
    wl.push(wpo.get_entry());
    int i = 0;
    std::ostringstream wto;
    bool first = true;
    while (!wl.empty()) {
      auto v = wl.top();
      wl.pop();
      for (auto w : wpo.get_successors(v)) {
        count[w]++;
        if (count[w] == wpo.get_num_preds(w)) {
          wl.push(w);
        }
      }

      auto& answer = lst[i++];
      EXPECT_EQ(answer.node, wpo.get_node(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.plain, wpo.is_plain(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.head, wpo.is_head(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.exit, wpo.is_exit(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.num_succs, wpo.get_successors(v).size())
          << wpo.get_node(v);
      EXPECT_EQ(answer.num_preds, wpo.get_num_preds(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.widen, wpo.is_widening_point(v)) << wpo.get_node(v);
      if (wpo.is_head(v)) {
        EXPECT_EQ(wpo.get_node(wpo.get_exit_of_head(v)), wpo.get_node(v))
            << wpo.get_node(v);
        if (!first) {
          wto << ' ';
        }
        wto << '(' << wpo.get_node(v);
      } else if (wpo.is_exit(v)) {
        EXPECT_EQ(answer.num_outer_preds, wpo.get_num_outer_preds(v).size())
            << wpo.get_node(v);
        EXPECT_EQ(wpo.get_node(wpo.get_head_of_exit(v)), wpo.get_node(v))
            << wpo.get_node(v);
        wto << ')';
      } else {
        if (!first) {
          wto << ' ';
        }
        wto << wpo.get_node(v);
      }
      if (first) {
        first = false;
      }
    }
    EXPECT_EQ(wto.str(), "1 (2 (3 4) (6 7 9 8) 5) 10");
  }
}

/*
 * This example illustrates the effect of setting the 'lifted' in the WPO
 * construction. While the resulting structures are both valid WPOs, only the
 * WPO with 'lifted' set during its construction has the same WTO as
 * Bourdoncle's when linearized. However, 'lifted' adds unnecessary orders
 * between WPO nodes, so it must be simply used when creating a WTO from a WPO.
 */
TEST(WeakPartialOrderingTest, exampleFromWpoPaperIrreducible) {
  {
    SimpleGraph2 g;
    g.add_edge("1", "2");
    g.add_edge("2", "3");
    g.add_edge("3", "2");
    g.add_edge("3", "4");
    g.add_edge("4", "3");
    g.add_edge("2", "5");
    g.add_edge("5", "4");
    g.add_edge("1", "6");
    g.add_edge("6", "4");

    WeakPartialOrdering<std::string> wpo(
        "1", [&g](const std::string& n) { return g.successors(n); }, false);

    EXPECT_FALSE(wpo.is_from_outside("3", "4"));
    EXPECT_FALSE(wpo.is_from_outside("2", "3"));
    EXPECT_TRUE(wpo.is_from_outside("2", "6"));
    EXPECT_TRUE(wpo.is_from_outside("3", "6"));
    EXPECT_TRUE(wpo.is_from_outside("3", "5"));
    EXPECT_FALSE(wpo.is_from_outside("2", "5"));

    EXPECT_EQ(8, wpo.size());

    // node, plain, head, exit, num_succs, num_preds, num_outer_preds
    Answer lst[] = {
        {"1", true, false, false, 2, 0, 0, false},
        {"2", false, true, false, 2, 1, 0, false},
        {"3", false, true, false, 1, 1, 0, true},
        {"5", true, false, false, 1, 1, 0, false},
        {"6", true, false, false, 1, 1, 0, false},
        {"4", true, false, false, 1, 3, 0, false},
        {"3", false, false, true, 1, 1, 2, false},
        {"2", false, false, true, 0, 1, 2, false},
    };

    std::unordered_map<WpoIdx, uint32_t> count(wpo.size());
    std::stack<WpoIdx> wl;
    wl.push(wpo.get_entry());
    int i = 0;
    std::ostringstream wto;
    bool first = true;
    while (!wl.empty()) {
      auto v = wl.top();
      wl.pop();
      for (auto w : wpo.get_successors(v)) {
        count[w]++;
        if (count[w] == wpo.get_num_preds(w)) {
          wl.push(w);
        }
      }

      auto& answer = lst[i++];
      EXPECT_EQ(answer.node, wpo.get_node(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.plain, wpo.is_plain(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.head, wpo.is_head(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.exit, wpo.is_exit(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.num_succs, wpo.get_successors(v).size())
          << wpo.get_node(v);
      EXPECT_EQ(answer.num_preds, wpo.get_num_preds(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.widen, wpo.is_widening_point(v)) << wpo.get_node(v);
      if (wpo.is_head(v)) {
        EXPECT_EQ(wpo.get_node(wpo.get_exit_of_head(v)), wpo.get_node(v))
            << wpo.get_node(v);
        if (!first) {
          wto << ' ';
        }
        wto << '(' << wpo.get_node(v);
      } else if (wpo.is_exit(v)) {
        EXPECT_EQ(answer.num_outer_preds, wpo.get_num_outer_preds(v).size())
            << wpo.get_node(v);
        EXPECT_EQ(wpo.get_node(wpo.get_head_of_exit(v)), wpo.get_node(v))
            << wpo.get_node(v);
        wto << ')';
      } else {
        if (!first) {
          wto << ' ';
        }
        wto << wpo.get_node(v);
      }
      if (first) {
        first = false;
      }
    }
    EXPECT_EQ(wto.str(), "1 (2 (3 5 6 4))");
  }
  {
    SimpleGraph2 g;
    g.add_edge("1", "2");
    g.add_edge("2", "3");
    g.add_edge("3", "2");
    g.add_edge("3", "4");
    g.add_edge("4", "3");
    g.add_edge("2", "5");
    g.add_edge("5", "4");
    g.add_edge("1", "6");
    g.add_edge("6", "4");

    WeakPartialOrdering<std::string> wpo(
        "1", [&g](const std::string& n) { return g.successors(n); }, true);
    //    "1 6 (2 5 (3 4))"

    EXPECT_FALSE(wpo.is_from_outside("3", "4"));
    EXPECT_FALSE(wpo.is_from_outside("2", "3"));
    EXPECT_TRUE(wpo.is_from_outside("2", "6"));
    EXPECT_TRUE(wpo.is_from_outside("3", "6"));
    EXPECT_TRUE(wpo.is_from_outside("3", "5"));
    EXPECT_FALSE(wpo.is_from_outside("2", "5"));

    EXPECT_EQ(8, wpo.size());

    // node, plain, head, exit, num_succs, num_preds, num_outer_preds
    Answer lst[] = {
        {"1", true, false, false, 2, 0, 0, false},
        {"6", true, false, false, 1, 1, 0, false},
        {"2", false, true, false, 2, 2, 0, false},
        {"5", true, false, false, 1, 1, 0, false},
        {"3", false, true, false, 1, 2, 0, true},
        {"4", true, false, false, 1, 1, 0, false},
        {"3", false, false, true, 1, 1, 1, false},
        {"2", false, false, true, 0, 1, 1, false},
    };

    std::unordered_map<WpoIdx, uint32_t> count(wpo.size());
    std::stack<WpoIdx> wl;
    wl.push(wpo.get_entry());
    int i = 0;
    std::ostringstream wto;
    bool first = true;
    while (!wl.empty()) {
      auto v = wl.top();
      wl.pop();
      for (auto w : wpo.get_successors(v)) {
        count[w]++;
        if (count[w] == wpo.get_num_preds(w)) {
          wl.push(w);
        }
      }

      auto& answer = lst[i++];
      EXPECT_EQ(answer.node, wpo.get_node(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.plain, wpo.is_plain(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.head, wpo.is_head(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.exit, wpo.is_exit(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.num_succs, wpo.get_successors(v).size())
          << wpo.get_node(v);
      EXPECT_EQ(answer.num_preds, wpo.get_num_preds(v)) << wpo.get_node(v);
      EXPECT_EQ(answer.widen, wpo.is_widening_point(v)) << wpo.get_node(v);
      if (wpo.is_head(v)) {
        EXPECT_EQ(wpo.get_node(wpo.get_exit_of_head(v)), wpo.get_node(v))
            << wpo.get_node(v);
        if (!first) {
          wto << ' ';
        }
        wto << '(' << wpo.get_node(v);
      } else if (wpo.is_exit(v)) {
        EXPECT_EQ(answer.num_outer_preds, wpo.get_num_outer_preds(v).size())
            << wpo.get_node(v);
        EXPECT_EQ(wpo.get_node(wpo.get_head_of_exit(v)), wpo.get_node(v))
            << wpo.get_node(v);
        wto << ')';
      } else {
        if (!first) {
          wto << ' ';
        }
        wto << wpo.get_node(v);
      }
      if (first) {
        first = false;
      }
    }
    EXPECT_EQ(wto.str(), "1 6 (2 5 (3 4))");
  }
}
