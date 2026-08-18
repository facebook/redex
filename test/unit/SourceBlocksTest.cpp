/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SourceBlocks.h"
#include "Debug.h"
#include "SourceBlocksViolations.h"

#include <atomic>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "Creators.h"
#include "DexClass.h"
#include "IRAssembler.h"
#include "Inliner.h"
#include "MetricsSink.h"
#include "RedexContext.h"
#include "RedexTest.h"
#include "Show.h"
#include "SourceBlocksViolations.h"

using namespace cfg;
using namespace source_blocks;

class SourceBlocksTest : public RedexTest {
 public:
  void SetUp() override { g_redex->set_sb_interaction_index({{"Fake", 0}}); }

  static DexMethod* create_method(const std::string& class_name = "LFoo",
                                  const std::string& code = "((return-void))") {
    // Create a totally new class.
    size_t c = s_counter.fetch_add(1);
    std::string name = class_name + std::to_string(c) + ";";
    ClassCreator cc{DexType::make_type(name)};
    cc.set_super(type::java_lang_Object());

    // Empty code isn't really legal. But it does not matter for us.
    auto* m = DexMethod::make_method(name + ".bar:()V")
                  ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                                  assembler::ircode_from_string(code), false);
    m->set_deobfuscated_name(show(m));
    cc.add_method(m);

    cc.create();

    return m;
  }

  static std::string get_blocks_as_txt(const std::vector<Block*>& l) {
    std::ostringstream oss;
    bool first = true;
    for (auto* block : l) {
      if (first) {
        first = false;
      } else {
        oss << "\n";
      }
      oss << "B" << block->id() << ":";
      auto vec = gather_source_blocks(block);
      for (auto* sb : vec) {
        oss << " " << show(sb->src) << "@" << sb->id;
        if (sb->vals_size > 0) {
          oss << "(";
          bool first_val = true;
          for (size_t i = 0; i < sb->vals_size; i++) {
            const auto& val = sb->get_at(i);
            if (!first_val) {
              oss << "|";
            }
            first_val = false;
            if (val) {
              oss << val->val << ":" << val->appear100;
            } else {
              oss << "x";
            }
          }
          oss << ")";
        }
      }
    }
    return remove_count(oss.str());
  }

  static std::string remove_count(const std::string& str) {
    // NOLINTNEXTLINE
    std::regex re("L[A-Z][a-z]*\\([0-9][0-9]*\\);", std::regex::basic);
    std::string res = str;
    for (size_t i = 0; i != 100; ++i) {
      std::smatch match;
      // NOLINTNEXTLINE
      if (!std::regex_search(res, match, re)) {
        break;
      }
      size_t pos = match.position(1);
      size_t length = match.length(1);
      res.replace(pos, length, "");
    }
    return res;
  }

  static void strip_source_blocks(ControlFlowGraph& cfg) {
    for (auto* b : cfg.blocks()) {
      auto it = b->begin();
      while (it != b->end()) {
        if (it->type == MFLOW_SOURCE_BLOCK) {
          b->remove_mie(it);
          it = b->begin();
          continue;
        }
        ++it;
      }
    }
  }

  static std::string replace_all(std::string in,
                                 const std::string& txt_in,
                                 const std::string& txt_out) {
    for (;;) {
      size_t pos = in.find(txt_in);
      if (pos == std::string::npos) {
        break;
      }
      in.replace(pos, txt_in.size(), txt_out);
    }
    return in;
  }

  static std::vector<source_blocks::ProfileData> single_profile(
      const std::string& p) {
    return std::vector<ProfileData>{std::make_pair(p, std::nullopt)};
  }

 private:
  static std::atomic<size_t> s_counter;
};
std::atomic<size_t> SourceBlocksTest::s_counter{0};

// The shared count-cutoff helpers underpin both the inliner's hot-callsite
// lever and the ArtProfileWriter NeverInline veto (rank), and the outliner's
// block_count_hot_coverage (mass); these tests pin their numeric core.
TEST_F(SourceBlocksTest, rank_cutoff_for_percentile) {
  const float kInf = std::numeric_limits<float>::infinity();
  // percentile >= 100 selects nothing (+inf); <= 0 selects all (-inf); empty ->
  // +inf.
  {
    std::vector<float> v{1, 2, 3, 4};
    EXPECT_EQ(rank_cutoff_for_percentile(v, 100), kInf);
  }
  {
    std::vector<float> v{1, 2, 3, 4};
    EXPECT_EQ(rank_cutoff_for_percentile(v, 0), -kInf);
  }
  {
    std::vector<float> v;
    EXPECT_EQ(rank_cutoff_for_percentile(v, 50), kInf);
  }
  // p95 = hottest 5%... here p75 = hottest 25% of {1,2,3,4}: idx =
  // floor(0.75*4) = 3 -> sorted[3] = 4.
  {
    std::vector<float> v{4, 1, 3, 2};
    EXPECT_FLOAT_EQ(rank_cutoff_for_percentile(v, 75), 4.0f);
  }
  // p50 = hottest 50%: idx = floor(0.5*4) = 2 -> sorted[2] = 3.
  {
    std::vector<float> v{4, 1, 3, 2};
    EXPECT_FLOAT_EQ(rank_cutoff_for_percentile(v, 50), 3.0f);
  }
  // Single element: any in-range percentile -> that element (idx clamps to 0).
  {
    std::vector<float> v{7};
    EXPECT_FLOAT_EQ(rank_cutoff_for_percentile(v, 50), 7.0f);
  }
}

TEST_F(SourceBlocksTest, mass_coverage_cutoff) {
  // vals 100,10,1,1,1 (mass 113). At 0.9 the target is 101.7: 100 alone is
  // short, +10 reaches 110 -> gate 10 (only val > 10 stays protected).
  {
    std::vector<float> v{1, 100, 1, 10, 1};
    EXPECT_FLOAT_EQ(mass_coverage_cutoff(v, 0.9f), 10.0f);
  }
  // coverage >= 1 -> protect every covered block -> gate 0.
  {
    std::vector<float> v{1, 10, 100};
    EXPECT_FLOAT_EQ(mass_coverage_cutoff(v, 1.0f), 0.0f);
  }
  // coverage <= 0 -> protect nothing -> gate = max val.
  {
    std::vector<float> v{1, 10, 100};
    EXPECT_FLOAT_EQ(mass_coverage_cutoff(v, 0.0f), 100.0f);
  }
  // empty distribution -> gate 0.
  {
    std::vector<float> v;
    EXPECT_FLOAT_EQ(mass_coverage_cutoff(v, 0.5f), 0.0f);
  }
}

TEST_F(SourceBlocksTest, max_val_over_interactions) {
  // No source block -> nullopt.
  EXPECT_FALSE(max_val_over_interactions(nullptr).has_value());
  // Max over ALL interaction slots, not just slot 0.
  using Val = SourceBlock::Val;
  SourceBlock sb(nullptr, 0, {Val(1, 1), Val(5, 1), Val(3, 1)});
  auto m = max_val_over_interactions(&sb);
  ASSERT_TRUE(m.has_value());
  EXPECT_FLOAT_EQ(*m, 5.0f);
}

TEST_F(SourceBlocksTest, minimal_serialize) {
  auto* method = create_method();
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();

  ASSERT_EQ(cfg.num_blocks(), 1u);

  auto res = insert_source_blocks(method, &cfg, {},
                                  /*serialize=*/true);

  EXPECT_EQ(res.block_count, 1u);
  EXPECT_EQ(res.serialized, "(0)");
}

TEST_F(SourceBlocksTest, visit_in_order_rec_vs_iter) {
  auto* method = create_method();
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();

  ASSERT_EQ(cfg.num_blocks(), 1u);
  auto* b = cfg.blocks()[0];

  // We're gonna just focus on blocks and edges, no instruction constraints.
  auto* b1 = cfg.create_block();
  auto* b2 = cfg.create_block();
  auto* b3 = cfg.create_block();
  auto* b4 = cfg.create_block();

  cfg.add_edge(b, b1, EDGE_GOTO);
  cfg.add_edge(b, b2, EDGE_BRANCH);
  cfg.add_edge(b1, b3, EDGE_GOTO);
  cfg.add_edge(b2, b3, EDGE_GOTO);
  cfg.add_edge(b1, b4, method->get_class(), 0);
  cfg.add_edge(b4, b3, EDGE_GOTO);

  struct Recorder {
    struct Event {
      const Block* block_start{nullptr};
      const Block* block_end{nullptr};
      const Block* const edge_src{nullptr};
      const Edge* edge{nullptr};
      Event(const Block* block_start,
            const Block* block_end,
            const Block* edge_src,
            const Edge* edge)
          : block_start(block_start),
            block_end(block_end),
            edge_src(edge_src),
            edge(edge) {}

      bool operator==(const Event& other) const {
        return block_start == other.block_start &&
               block_end == other.block_end && edge_src == other.edge_src &&
               edge == other.edge;
      }

      std::string to_string() const {
        if (block_start != nullptr) {
          return "S" + std::to_string(block_start->id());
        }
        if (block_end != nullptr) {
          return "E" + std::to_string(block_end->id());
        }
        redex_assert(edge_src != nullptr && edge != nullptr);
        return "e" + std::to_string(edge_src->id()) + "-" +
               std::to_string(edge->type()) + "-" +
               std::to_string(edge->target()->id());
      }
    };

    std::vector<Event> events;

    void block_start(const Block* b) {
      events.emplace_back(b, nullptr, nullptr, nullptr);
    }
    void block_end(const Block* b) {
      events.emplace_back(nullptr, b, nullptr, nullptr);
    }
    void edge(const Block* src, const Edge* e) {
      events.emplace_back(nullptr, nullptr, src, e);
    }

    bool operator==(const Recorder& other) const {
      return events == other.events;
    }

    std::string to_string() const {
      return "[" + [&]() {
        std::string tmp;
        for (const auto& e : events) {
          tmp += e.to_string() + ",";
        }
        return tmp;
      }() + "]";
    }
  };

  Recorder recursive;
  impl::visit_in_order_rec(
      &cfg, [&recursive](auto b) { recursive.block_start(b); },
      [&recursive](auto b, auto e) { recursive.edge(b, e); },
      [&recursive](auto b) { recursive.block_end(b); });

  Recorder iterative;
  impl::visit_in_order(
      &cfg, [&iterative](auto b) { iterative.block_start(b); },
      [&iterative](auto b, auto e) { iterative.edge(b, e); },
      [&iterative](auto b) { iterative.block_end(b); });

  EXPECT_EQ(recursive, iterative)
      << "Recursive: "
      << recursive.to_string() + "\nIterative: " + iterative.to_string();
}

TEST_F(SourceBlocksTest, complex_serialize) {
  auto* method = create_method();
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();

  ASSERT_EQ(cfg.num_blocks(), 1u);
  auto* b = cfg.blocks()[0];

  // We're gonna just focus on blocks and edges, no instruction constraints.
  auto* b1 = cfg.create_block();
  auto* b2 = cfg.create_block();
  auto* b3 = cfg.create_block();
  auto* b4 = cfg.create_block();

  cfg.add_edge(b, b1, EDGE_GOTO);
  cfg.add_edge(b, b2, EDGE_BRANCH);
  cfg.add_edge(b1, b3, EDGE_GOTO);
  cfg.add_edge(b2, b3, EDGE_GOTO);
  cfg.add_edge(b1, b4, method->get_class(), 0);
  cfg.add_edge(b4, b3, EDGE_GOTO);

  auto res = insert_source_blocks(method, &cfg, {},
                                  /*serialize=*/true);

  EXPECT_EQ(res.block_count, 5u);
  EXPECT_EQ(res.serialized, "(0 g(1 g(2) t(3 g)) b(4 g))");
  EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), R"(B0: LFoo;.bar:()V@0
B1: LFoo;.bar:()V@1
B2: LFoo;.bar:()V@4
B3: LFoo;.bar:()V@2
B4: LFoo;.bar:()V@3)");
}

TEST_F(SourceBlocksTest, complex_deserialize) {
  auto* method = create_method();
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();

  ASSERT_EQ(cfg.num_blocks(), 1u);
  auto* b = cfg.blocks()[0];

  // We're gonna just focus on blocks and edges, no instruction constraints.
  auto* b1 = cfg.create_block();
  auto* b2 = cfg.create_block();
  auto* b3 = cfg.create_block();
  auto* b4 = cfg.create_block();

  cfg.add_edge(b, b1, EDGE_GOTO);
  cfg.add_edge(b, b2, EDGE_BRANCH);
  cfg.add_edge(b1, b3, EDGE_GOTO);
  cfg.add_edge(b2, b3, EDGE_GOTO);
  cfg.add_edge(b1, b4, method->get_class(), 0);
  cfg.add_edge(b4, b3, EDGE_GOTO);

  auto profile = single_profile(
      "(0.1:0.5 g(0.2:0.4 g(0.3:0.3) t(0.4:0.2 g)) b(0.5:0.1 g))");

  auto res = insert_source_blocks(method, &cfg, profile,
                                  /*serialize=*/true);

  EXPECT_EQ(res.block_count, 5u);
  EXPECT_EQ(res.serialized, "(0 g(1 g(2) t(3 g)) b(4 g))");
  EXPECT_TRUE(res.profile_success);
  EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}),
            R"(B0: LFoo;.bar:()V@0(0.1:0.5)
B1: LFoo;.bar:()V@1(0.2:0.4)
B2: LFoo;.bar:()V@4(0.5:0.1)
B3: LFoo;.bar:()V@2(0.3:0.3)
B4: LFoo;.bar:()V@3(0.4:0.2))");
}

TEST_F(SourceBlocksTest, complex_deserialize_global_default) {
  auto* method = create_method();
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();

  ASSERT_EQ(cfg.num_blocks(), 1u);
  auto* b = cfg.blocks()[0];

  // We're gonna just focus on blocks and edges, no instruction constraints.
  auto* b1 = cfg.create_block();
  auto* b2 = cfg.create_block();
  auto* b3 = cfg.create_block();
  auto* b4 = cfg.create_block();

  cfg.add_edge(b, b1, EDGE_GOTO);
  cfg.add_edge(b, b2, EDGE_BRANCH);
  cfg.add_edge(b1, b3, EDGE_GOTO);
  cfg.add_edge(b2, b3, EDGE_GOTO);
  cfg.add_edge(b1, b4, method->get_class(), 0);
  cfg.add_edge(b4, b3, EDGE_GOTO);

  auto profile = single_profile(
      "(0.1:0.5 g(0.2:0.4 g(0.3:0.3) t(0.4:0.2 g)) b(0.5:0.1 g))");

  auto res = insert_custom_source_blocks(&method->get_deobfuscated_name(), &cfg,
                                         profile,
                                         /*serialize=*/true,
                                         /*insert_after_excs=*/false,
                                         /*enable_fuzzing=*/false);

  EXPECT_EQ(res.block_count, 5u);
  EXPECT_EQ(res.serialized, "(0 g(1 g(2) t(3 g)) b(4 g))");
  EXPECT_TRUE(res.profile_success);
  EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}),
            R"(B0: LFoo;.bar:()V@0(1:1)
B1: LFoo;.bar:()V@1(1:1)
B2: LFoo;.bar:()V@4(1:1)
B3: LFoo;.bar:()V@2(1:1)
B4: LFoo;.bar:()V@3(1:1))");
}

TEST_F(SourceBlocksTest, complex_deserialize_default) {
  auto* method = create_method();
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();

  ASSERT_EQ(cfg.num_blocks(), 1u);
  auto* b = cfg.blocks()[0];

  // We're gonna just focus on blocks and edges, no instruction constraints.
  auto* b1 = cfg.create_block();
  auto* b2 = cfg.create_block();
  auto* b3 = cfg.create_block();
  auto* b4 = cfg.create_block();

  cfg.add_edge(b, b1, EDGE_GOTO);
  cfg.add_edge(b, b2, EDGE_BRANCH);
  cfg.add_edge(b1, b3, EDGE_GOTO);
  cfg.add_edge(b2, b3, EDGE_GOTO);
  cfg.add_edge(b1, b4, method->get_class(), 0);
  cfg.add_edge(b4, b3, EDGE_GOTO);

  auto profile = std::vector<ProfileData>{SourceBlock::Val(123, 456)};

  auto res = insert_source_blocks(method, &cfg, profile,
                                  /*serialize=*/true);

  EXPECT_EQ(res.block_count, 5u);
  EXPECT_EQ(res.serialized, "(0 g(1 g(2) t(3 g)) b(4 g))");
  EXPECT_TRUE(res.profile_success);
  EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}),
            R"(B0: LFoo;.bar:()V@0(123:456)
B1: LFoo;.bar:()V@1(123:456)
B2: LFoo;.bar:()V@4(123:456)
B3: LFoo;.bar:()V@2(123:456)
B4: LFoo;.bar:()V@3(123:456))");
}

TEST_F(SourceBlocksTest, complex_deserialize_failure) {
  auto* method = create_method();
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();

  ASSERT_EQ(cfg.num_blocks(), 1u);
  auto* b = cfg.blocks()[0];

  // We're gonna just focus on blocks and edges, no instruction constraints.
  auto* b1 = cfg.create_block();
  auto* b2 = cfg.create_block();
  auto* b3 = cfg.create_block();
  auto* b4 = cfg.create_block();

  cfg.add_edge(b, b1, EDGE_GOTO);
  cfg.add_edge(b, b2, EDGE_BRANCH);
  cfg.add_edge(b1, b3, EDGE_GOTO);
  cfg.add_edge(b2, b3, EDGE_GOTO);
  cfg.add_edge(b1, b4, method->get_class(), 0);
  cfg.add_edge(b4, b3, EDGE_GOTO);

  const std::string kSerializedExp = R"(B0: LFoo;.bar:()V@0(x)
B1: LFoo;.bar:()V@1(x)
B2: LFoo;.bar:()V@4(x)
B3: LFoo;.bar:()V@2(x)
B4: LFoo;.bar:()V@3(x))";

  // Change the profiles a bit so they should not match.
  {
    auto profile = single_profile(
        "(0.1:0.0 b(0.2:0.0 g(0.3:0.0) t(0.4:0.0 g)) b(0.5:0.0 g))");
    auto res = insert_source_blocks(method, &cfg, profile,
                                    /*serialize=*/true);
    EXPECT_FALSE(res.profile_success);
    EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), kSerializedExp);
    strip_source_blocks(cfg);
  }

  {
    auto profile = single_profile(
        "(0.1:0.0 g(0.2:0.0 t(0.3:0.0) t(0.4:0.0 g)) b(0.5:0.0 g))");
    auto res = insert_source_blocks(method, &cfg, profile,
                                    /*serialize=*/true);
    EXPECT_FALSE(res.profile_success);
    EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), kSerializedExp);
    strip_source_blocks(cfg);
  }

  {
    auto profile =
        single_profile("(0.1:0.0 g(0.2:0.0 g(0.3:0.0)) b(0.5:0.0 g))");
    auto res = insert_source_blocks(method, &cfg, profile,
                                    /*serialize=*/true);
    EXPECT_FALSE(res.profile_success);
    EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), kSerializedExp);
    strip_source_blocks(cfg);
  }

  {
    auto profile =
        single_profile("(0.1:0.0 g(0.2:0.0 g(0.3:0.0) t(0.4:0.0 g)))");
    auto res = insert_source_blocks(method, &cfg, profile,
                                    /*serialize=*/true);
    EXPECT_FALSE(res.profile_success);
    EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), kSerializedExp);
    strip_source_blocks(cfg);
  }

  // We want the exception message, not the abort line.
  auto extract_exc_msg = [](const std::exception& e) {
    std::string s(e.what());
    auto idx = s.find('\n');
    if (idx == std::string::npos) {
      return s;
    }
    return s.substr(idx + 1);
  };

  // Nothing parseable as float (and not 'x').
  {
    auto profile = single_profile("(hello:world g(0.2 g(0.3) t(0.4 g)))");
    try {
      insert_source_blocks(method, &cfg, profile, /*serialize=*/true);
      ADD_FAILURE() << "Expected exception.";
    } catch (const std::exception&) {
      // This is not ours, but from std::stof. Message is not well-specified, I
      // think.
    }
  }
  // Not fully parseable as float (and not 'x').
  {
    auto profile = single_profile("(0hello:world g(0.2 g(0.3) t(0.4 g)))");
    try {
      insert_source_blocks(method, &cfg, profile, /*serialize=*/true);
      ADD_FAILURE() << "Expected exception.";
    } catch (const std::exception& e) {
      EXPECT_EQ(extract_exc_msg(e),
                "Did not find separating ':' in 0hello:world");
    }
  }
  // Missing appear100.
  {
    auto profile = single_profile("(0.1 g(0.2 g(0.3) t(0.4 g)))");
    try {
      insert_source_blocks(method, &cfg, profile, /*serialize=*/true);
      ADD_FAILURE() << "Expected exception.";
    } catch (const std::exception& e) {
      EXPECT_EQ(extract_exc_msg(e), "Could not find separator of 0.1");
    }
  }
  // Wrong character.
  {
    auto profile = single_profile("(0.1/0.0 g(0.2/0 g(0.3/0) t(0.4/0 g)))");
    try {
      insert_source_blocks(method, &cfg, profile, /*serialize=*/true);
      ADD_FAILURE() << "Expected exception.";
    } catch (const std::exception& e) {
      EXPECT_EQ(extract_exc_msg(e), "Did not find separating ':' in 0.1/0.0");
    }
  }
  // Not a float in appear.
  {
    auto profile = single_profile("(0:0world g(0.2 g(0.3) t(0.4 g)))");
    try {
      insert_source_blocks(method, &cfg, profile, /*serialize=*/true);
      ADD_FAILURE() << "Expected exception.";
    } catch (const std::exception& e) {
      EXPECT_EQ(extract_exc_msg(e),
                "Could not parse second part of 0:0world as float");
    }
  }
}

TEST_F(SourceBlocksTest, complex_deserialize_failure_error_val) {
  auto* method = create_method();
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();

  ASSERT_EQ(cfg.num_blocks(), 1u);
  auto* b = cfg.blocks()[0];

  // We're gonna just focus on blocks and edges, no instruction constraints.
  auto* b1 = cfg.create_block();
  auto* b2 = cfg.create_block();
  auto* b3 = cfg.create_block();
  auto* b4 = cfg.create_block();

  cfg.add_edge(b, b1, EDGE_GOTO);
  cfg.add_edge(b, b2, EDGE_BRANCH);
  cfg.add_edge(b1, b3, EDGE_GOTO);
  cfg.add_edge(b2, b3, EDGE_GOTO);
  cfg.add_edge(b1, b4, method->get_class(), 0);
  cfg.add_edge(b4, b3, EDGE_GOTO);

  const std::string kSerializedExp = R"(B0: LFoo;.bar:()V@0(123:456)
B1: LFoo;.bar:()V@1(123:456)
B2: LFoo;.bar:()V@4(123:456)
B3: LFoo;.bar:()V@2(123:456)
B4: LFoo;.bar:()V@3(123:456))";

  auto profile = std::vector<ProfileData>{std::make_pair(
      std::string("(0.1:0.0 b(0.2:0.0 g(0.3:0.0) t(0.4:0.0 g)) b(0.5:0.0 g))"),
      SourceBlock::Val(123, 456))};
  auto res = insert_source_blocks(method, &cfg, profile,
                                  /*serialize=*/true);
  EXPECT_FALSE(res.profile_success);
  EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), kSerializedExp);
}

TEST_F(SourceBlocksTest, inline_normalization) {
  auto* foo_method = create_method("LFoo");
  auto* bar_method = create_method("LBar");

  constexpr const char* kCode = R"(
    (
      (const v0 0)
      (if-eqz v0 :true)
      (goto :end)

      (:true)
      (invoke-static () "LBarX;.bar:()I")

      (:end)
      (return-void)
    )
  )";

  foo_method->set_code(assembler::ircode_from_string(
      replace_all(kCode, "LBarX;", show(bar_method->get_class()))));

  foo_method->get_code()->build_cfg();
  auto& foo_cfg = foo_method->get_code()->cfg();
  auto foo_profile = single_profile("(1.0:0.1 g(0.6:0.2) b(0.5:0.3 g))");
  auto res = insert_source_blocks(foo_method, &foo_cfg, foo_profile,
                                  /*serialize=*/true);
  EXPECT_TRUE(res.profile_success);

  bar_method->set_code(assembler::ircode_from_string(
      replace_all(kCode, "LBarX;", show(bar_method->get_class()))));

  bar_method->get_code()->build_cfg();
  auto& bar_cfg = bar_method->get_code()->cfg();
  auto bar_profile = single_profile("(1:0.1 g(0.4:0.2) b(0.2:0.3 g))");
  auto bar_res = insert_source_blocks(bar_method, &bar_cfg, bar_profile,
                                      /*serialize=*/true);
  EXPECT_TRUE(bar_res.profile_success);

  IRInstruction* invoke_insn = nullptr;
  for (auto& mie : cfg::InstructionIterable(foo_cfg)) {
    if (mie.insn->opcode() == OPCODE_INVOKE_STATIC) {
      invoke_insn = mie.insn;
      break;
    }
  }
  ASSERT_NE(invoke_insn, nullptr);
  inliner::inline_with_cfg(foo_method, bar_method, invoke_insn,
                           /* needs_receiver_cast */ nullptr,
                           /* needs_init_class */ nullptr, 1);

  // Values of LBar; should be halved.

  EXPECT_EQ(get_blocks_as_txt(foo_cfg.blocks()), R"(B0: LFoo;.bar:()V@0(1:0.1)
B2: LFoo;.bar:()V@2(0.5:0.3)
B3: LFoo;.bar:()V@1(0.6:0.2)
B4: LBar;.bar:()V@0(0.5:0.1)
B5: LBar;.bar:()V@2(0.1:0.3)
B6: LBar;.bar:()V@1(0.2:0.2))");
}

// The whole-CFG scale is a SHARE of the dominated block's executions, so it is
// at most 1. A dominating value larger than the dominated one -- incomplete
// tracking, or a denominator pinned to a positive-magnitude floor -- must not
// inflate the scaled body.
TEST_F(SourceBlocksTest, normalize_cfg_clamps_factor_at_one) {
  auto* bar_method = create_method("LBar");

  constexpr const char* kCode = R"(
    (
      (const v0 0)
      (if-eqz v0 :true)
      (goto :end)

      (:true)
      (const v1 1)

      (:end)
      (return-void)
    )
  )";

  bar_method->set_code(assembler::ircode_from_string(kCode));
  bar_method->get_code()->build_cfg();
  auto& bar_cfg = bar_method->get_code()->cfg();
  auto bar_profile = single_profile("(1:0.1 g(0.4:0.2) b(0.2:0.3 g))");
  auto res = insert_source_blocks(bar_method, &bar_cfg, bar_profile,
                                  /*serialize=*/true);
  EXPECT_TRUE(res.profile_success);

  const auto before = get_blocks_as_txt(bar_cfg.blocks());

  // Entry reads 1.0, so an unclamped factor would be 1000 and every block in
  // the CFG would be multiplied by it.
  const auto* s = DexString::make_string("dominating");
  SourceBlock dominating(
      s, 0, std::vector<SourceBlock::Val>{SourceBlock::Val(1000, 0.1)});
  source_blocks::normalize::normalize(bar_cfg, &dominating,
                                      /*interactions=*/1);

  EXPECT_EQ(get_blocks_as_txt(bar_cfg.blocks()), before);
}

// A zero factor must survive the floor: inlining a HOT callee into a COLD
// caller has to leave the inlined blocks cold. `kMinPositiveCount` only lifts
// values that are still positive after the scale, so `val * 0 == 0` is left
// alone -- the floor can raise a small value, never resurrect a zeroed one.
TEST_F(SourceBlocksTest, inline_normalization_cold_caller_stays_cold) {
  auto* foo_method = create_method("LFoo");
  auto* bar_method = create_method("LBar");

  constexpr const char* kCode = R"(
    (
      (const v0 0)
      (if-eqz v0 :true)
      (goto :end)

      (:true)
      (invoke-static () "LBarX;.bar:()I")

      (:end)
      (return-void)
    )
  )";

  foo_method->set_code(assembler::ircode_from_string(
      replace_all(kCode, "LBarX;", show(bar_method->get_class()))));
  foo_method->get_code()->build_cfg();
  auto& foo_cfg = foo_method->get_code()->cfg();
  // The block holding the invoke has val 0: a COLD callsite.
  auto foo_profile = single_profile("(1.0:0.1 g(0.6:0.2) b(0:0.3 g))");
  auto res = insert_source_blocks(foo_method, &foo_cfg, foo_profile,
                                  /*serialize=*/true);
  EXPECT_TRUE(res.profile_success);

  bar_method->set_code(assembler::ircode_from_string(
      replace_all(kCode, "LBarX;", show(bar_method->get_class()))));
  bar_method->get_code()->build_cfg();
  auto& bar_cfg = bar_method->get_code()->cfg();
  // The callee is HOT.
  auto bar_profile = single_profile("(1:0.1 g(0.4:0.2) b(0.2:0.3 g))");
  auto bar_res = insert_source_blocks(bar_method, &bar_cfg, bar_profile,
                                      /*serialize=*/true);
  EXPECT_TRUE(bar_res.profile_success);

  IRInstruction* invoke_insn = nullptr;
  for (auto& mie : cfg::InstructionIterable(foo_cfg)) {
    if (mie.insn->opcode() == OPCODE_INVOKE_STATIC) {
      invoke_insn = mie.insn;
      break;
    }
  }
  ASSERT_NE(invoke_insn, nullptr);
  inliner::inline_with_cfg(foo_method, bar_method, invoke_insn,
                           /* needs_receiver_cast */ nullptr,
                           /* needs_init_class */ nullptr, 1);

  // Every inlined LBar; block is 0, not kMinPositiveCount. appear100 is not
  // scaled, so it is carried over untouched.
  EXPECT_EQ(get_blocks_as_txt(foo_cfg.blocks()), R"(B0: LFoo;.bar:()V@0(1:0.1)
B2: LFoo;.bar:()V@2(0:0.3)
B3: LFoo;.bar:()V@1(0.6:0.2)
B4: LBar;.bar:()V@0(0:0.1)
B5: LBar;.bar:()V@2(0:0.3)
B6: LBar;.bar:()V@1(0:0.2))");
}

// The post-scaling guard exists only to stop an underflow from turning a hot
// block cold; it must NOT distort the magnitude. A factor small enough to
// underflow the product to 0.0f leaves a strictly-positive val, and one far
// below the old 1e-3 floor.
TEST_F(SourceBlocksTest,
       normalize_guard_prevents_underflow_without_distorting) {
  const auto* s = DexString::make_string("x");

  // 1e-30 * 1e-20 == 1e-50, which is not representable and flushes to 0.0f.
  SourceBlock sb(s, 0,
                 std::vector<SourceBlock::Val>{SourceBlock::Val(1e-30f, 50)});
  source_blocks::normalize::normalize(&sb, 0, 1e-20f);
  ASSERT_TRUE(sb.get_val(0).has_value());
  EXPECT_GT(*sb.get_val(0), 0.0f); // guarded: still hot
  EXPECT_LT(*sb.get_val(0), 1e-3f); // not pinned to the old floor
  EXPECT_FLOAT_EQ(*sb.get_appear100(0), 50.0f); // appear100 is never scaled

  // An ordinary small factor is left exactly alone -- no floor, no rounding.
  SourceBlock sb2(s, 0,
                  std::vector<SourceBlock::Val>{SourceBlock::Val(1.0f, 50)});
  source_blocks::normalize::normalize(&sb2, 0, 1e-6f);
  ASSERT_TRUE(sb2.get_val(0).has_value());
  EXPECT_FLOAT_EQ(*sb2.get_val(0), 1e-6f);
}

// The kChainAndDom magnitude check compares a block against its immediate
// dominator, guarded on the block's sole inflow BEING that dominator. The
// guard reads `state.dom_block`, which the walk must set to whichever
// dominator supplied the SourceBlock it compares against.
//
// The existing integ fixture (IDomBlockCounting.idom) is a single top-level
// if/else, so its arms' immediate dominator IS the entry block -- the one case
// the walk always handled. This pins the ordinary case: an inner `if` whose
// branch block is NOT the entry. B3 below has exactly one predecessor, the
// inner branch block that dominates it, and runs twice as often, which cannot
// happen and must be reported.
TEST_F(SourceBlocksTest, chain_and_dom_magnitude_fires_below_entry_block) {
  auto* method = create_method("LChainDom");
  method->set_code(assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (.src_block "LChainDom;.bar:()V" 0 (1.0 1.0))
      (if-eqz v0 :end)

      (.src_block "LChainDom;.bar:()V" 1 (1.0 1.0))
      (if-eqz v0 :end)

      (.src_block "LChainDom;.bar:()V" 2 (2.0 1.0))
      (const v1 0)

      (:end)
      (.src_block "LChainDom;.bar:()V" 3 (1.0 1.0))
      (return-void)
    )
  )"));
  method->get_code()->build_cfg();

  auto violations = source_blocks::compute(
      source_blocks::ViolationsHelper::Violation::kChainAndDom,
      method->get_code()->cfg());
  EXPECT_EQ(violations, 1u);
}

TEST_F(SourceBlocksTest, serialize_exc_injected) {
  auto* foo_method = create_method("LFoo");

  constexpr const char* kCode = R"(
    (
      (const v0 0)
      (invoke-static () "LFooX;.bar:()V")
      (invoke-static () "LFooX;.bar2:()I")
      (move-result v1)
      (invoke-static () "LFooX;.bar:()V")

      (if-eqz v0 :true)
      (goto :end)

      (:true)
      (invoke-static () "LBarX;.bar:()I")

      (:end)

      (return-void)
    )
  )";

  foo_method->set_code(assembler::ircode_from_string(
      replace_all(kCode, "LFooX;", show(foo_method->get_class()))));

  foo_method->get_code()->build_cfg();
  auto& foo_cfg = foo_method->get_code()->cfg();
  auto res =
      insert_source_blocks(foo_method, &foo_cfg, {},
                           /*serialize=*/true, /*insert_after_excs=*/true);
  EXPECT_EQ(res.serialized, "(0(1)(2)(3) g(4) b(5 g))");
  EXPECT_EQ(
      get_blocks_as_txt(foo_cfg.blocks()),
      R"(B0: LFoo;.bar:()V@0 LFoo;.bar:()V@1 LFoo;.bar:()V@2 LFoo;.bar:()V@3
B2: LFoo;.bar:()V@5
B3: LFoo;.bar:()V@4)");
}

TEST_F(SourceBlocksTest, deserialize_exc_injected) {
  auto* foo_method = create_method("LFoo");

  constexpr const char* kCode = R"(
    (
      (const v0 0)
      (invoke-static () "LFooX;.bar:()V")
      (invoke-static () "LFooX;.bar2:()I")
      (move-result v1)
      (invoke-static () "LFooX;.bar:()V")

      (if-eqz v0 :true)
      (goto :end)

      (:true)
      (invoke-static () "LBarX;.bar:()I")

      (:end)

      (return-void)
    )
  )";

  foo_method->set_code(assembler::ircode_from_string(
      replace_all(kCode, "LFooX;", show(foo_method->get_class()))));

  foo_method->get_code()->build_cfg();
  auto& foo_cfg = foo_method->get_code()->cfg();
  auto profile = single_profile("(1:0(2:0)(3:0)(4:0) g(5:0) b(6:0 g))");
  auto res =
      insert_source_blocks(foo_method, &foo_cfg, profile,
                           /*serialize=*/true, /*insert_after_excs=*/true);
  EXPECT_TRUE(res.profile_success);
  EXPECT_EQ(res.serialized, "(0(1)(2)(3) g(4) b(5 g))");
  EXPECT_EQ(
      get_blocks_as_txt(foo_cfg.blocks()),
      R"(B0: LFoo;.bar:()V@0(1:0) LFoo;.bar:()V@1(2:0) LFoo;.bar:()V@2(3:0) LFoo;.bar:()V@3(4:0)
B2: LFoo;.bar:()V@5(6:0)
B3: LFoo;.bar:()V@4(5:0))");
}

TEST_F(SourceBlocksTest, deserialize_x) {
  auto* method = create_method();
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();

  ASSERT_EQ(cfg.num_blocks(), 1u);
  auto* b = cfg.blocks()[0];

  // We're gonna just focus on blocks and edges, no instruction constraints.
  auto* b1 = cfg.create_block();
  auto* b2 = cfg.create_block();
  auto* b3 = cfg.create_block();
  auto* b4 = cfg.create_block();

  cfg.add_edge(b, b1, EDGE_GOTO);
  cfg.add_edge(b, b2, EDGE_BRANCH);
  cfg.add_edge(b1, b3, EDGE_GOTO);
  cfg.add_edge(b2, b3, EDGE_GOTO);
  cfg.add_edge(b1, b4, method->get_class(), 0);
  cfg.add_edge(b4, b3, EDGE_GOTO);

  auto profile = single_profile("(0.1:0.1 g(x g(x) t(0.4:0.2 g)) b(x g))");

  auto res = insert_source_blocks(method, &cfg, profile,
                                  /*serialize=*/true);

  EXPECT_EQ(res.block_count, 5u);
  EXPECT_EQ(res.serialized, "(0 g(1 g(2) t(3 g)) b(4 g))");
  EXPECT_TRUE(res.profile_success);
  EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}),
            R"(B0: LFoo;.bar:()V@0(0.1:0.1)
B1: LFoo;.bar:()V@1(x)
B2: LFoo;.bar:()V@4(x)
B3: LFoo;.bar:()V@2(x)
B4: LFoo;.bar:()V@3(0.4:0.2))");
}

TEST_F(SourceBlocksTest, coalesce) {
  IRList::CONSECUTIVE_STYLE = IRList::ConsecutiveStyle::kChain;

  auto* foo_method = create_method("LFoo");

  constexpr const char* kCode = R"(
    (
      (const v0 0)
      (invoke-static () "LFooX;.bar:()V")
      (invoke-static () "LFooX;.bar2:()I")
      (move-result v1)
      (invoke-static () "LFooX;.bar:()V")

      (if-eqz v0 :true)
      (goto :end)

      (:true)
      (invoke-static () "LBarX;.bar:()I")

      (:end)

      (return-void)
    )
  )";

  foo_method->set_code(assembler::ircode_from_string(
      replace_all(kCode, "LFooX;", show(foo_method->get_class()))));

  foo_method->get_code()->build_cfg();
  {
    auto& foo_cfg = foo_method->get_code()->cfg();
    auto profile = single_profile("(1:0(2:0)(3:0)(4:0) g(5:0) b(6:0 g))");
    auto res =
        insert_source_blocks(foo_method, &foo_cfg, profile,
                             /*serialize=*/true, /*insert_after_excs=*/true);
    EXPECT_TRUE(res.profile_success);
    EXPECT_EQ(res.serialized, "(0(1)(2)(3) g(4) b(5 g))");

    EXPECT_EQ(
        get_blocks_as_txt(foo_cfg.blocks()),
        R"(B0: LFoo;.bar:()V@0(1:0) LFoo;.bar:()V@1(2:0) LFoo;.bar:()V@2(3:0) LFoo;.bar:()V@3(4:0)
B2: LFoo;.bar:()V@5(6:0)
B3: LFoo;.bar:()V@4(5:0))");
  }

  auto count_coalesced = [](auto* b) {
    size_t cnt{0};
    size_t sum{0};
    for (const auto& mie : *b) {
      if (mie.type != MFLOW_SOURCE_BLOCK) {
        continue;
      }
      size_t sb_cnt{0};
      for (auto* sb = mie.src_block.get(); sb != nullptr; sb = sb->next.get()) {
        ++sb_cnt;
      }
      sum += sb_cnt;
      if (sb_cnt > 1) {
        ++cnt;
      }
    }
    return std::make_pair(cnt, sum);
  };

  // Should not have coalesced.
  foo_method->get_code()->clear_cfg();
  foo_method->get_code()->build_cfg();
  {
    auto& foo_cfg = foo_method->get_code()->cfg();
    auto no_coalesced = count_coalesced(foo_cfg.entry_block());
    EXPECT_EQ(no_coalesced.first, 0);
    EXPECT_EQ(no_coalesced.second, 4);
  }

  // Delete the invokes.
  {
    auto& foo_cfg = foo_method->get_code()->cfg();
    auto* entry = foo_cfg.entry_block();
    std::vector<IRInstruction*> to_delete;
    for (auto& mie : ir_list::InstructionIterable(entry)) {
      if (mie.insn->opcode() == OPCODE_INVOKE_STATIC) {
        to_delete.push_back(mie.insn);
      }
    }
    ASSERT_FALSE(to_delete.empty());
    for (auto* insn : to_delete) {
      entry->remove_insn(foo_cfg.find_insn(insn, entry));
    }
  }
  // Clear & rebuild.
  foo_method->get_code()->clear_cfg();
  foo_method->get_code()->build_cfg();
  auto& foo_cfg = foo_method->get_code()->cfg();
  // Should have coalesced.
  auto coalesced = count_coalesced(foo_cfg.entry_block());
  EXPECT_EQ(coalesced.first, 1);
  EXPECT_EQ(coalesced.second, 4);
}

TEST_F(SourceBlocksTest, get_last_source_block_before) {
  auto* foo_method = create_method("LFoo");

  constexpr const char* kCode = R"(
    (
      (.src_block "LFoo;.bar:()V" 0)
      (const v0 0)
      (.src_block "LFoo;.bar:()V" 1)
      (const v1 1)
      (.src_block "LFoo;.bar:()V" 2)
      (const v2 2)
      (.src_block "LFoo;.bar:()V" 3)
      (const v3 3)

      (.src_block "LFoo;.bar:()V" 4)

      (return-void)
    )
  )";

  foo_method->set_code(assembler::ircode_from_string(kCode));

  foo_method->get_code()->build_cfg();

  auto* b = foo_method->get_code()->cfg().entry_block();

  for (auto it = b->begin(); it != b->end(); ++it) {
    if (it->type != MFLOW_OPCODE) {
      continue;
    }
    if (it->insn->opcode() == OPCODE_CONST) {
      auto num = static_cast<uint32_t>(it->insn->get_literal());
      auto* sb = source_blocks::get_last_source_block_before(b, it);
      EXPECT_NE(sb, nullptr);
      if (sb != nullptr) {
        EXPECT_EQ(sb->id, num);
      }
    }
  }
}

TEST_F(SourceBlocksTest, get_last_source_block_before_non_entry) {
  auto* foo_method = create_method("LFoo");

  constexpr const char* kCode = R"(
    (
      (const v0 0)
      (.src_block "LFoo;.bar:()V" 1)
      (const v1 1)
      (.src_block "LFoo;.bar:()V" 2)
      (const v2 2)
      (.src_block "LFoo;.bar:()V" 3)
      (const v3 3)

      (.src_block "LFoo;.bar:()V" 4)

      (return-void)
    )
  )";

  foo_method->set_code(assembler::ircode_from_string(kCode));

  foo_method->get_code()->build_cfg();

  auto* b = foo_method->get_code()->cfg().entry_block();

  for (auto it = b->begin(); it != b->end(); ++it) {
    if (it->type != MFLOW_OPCODE) {
      continue;
    }
    if (it->insn->opcode() == OPCODE_CONST) {
      auto num = static_cast<uint32_t>(it->insn->get_literal());
      auto* sb = source_blocks::get_last_source_block_before(b, it);
      if (num == 0) {
        EXPECT_EQ(sb, nullptr);
      } else {
        EXPECT_NE(sb, nullptr);
        if (sb != nullptr) {
          EXPECT_EQ(sb->id, num);
        }
      }
    }
  }
}

// dedup the diamond test code from the DedupBlocks unit tests
TEST_F(SourceBlocksTest, dedup_diamond_with_interactions) {
  g_redex->instrument_mode = true;
  IRList::CONSECUTIVE_STYLE = IRList::ConsecutiveStyle::kChain;
  DexMethod* method = create_method("diamond");

  const auto* str = R"(
    (
      (.src_block "LFoo;.bar:()V" 1 (1.0 1.0) (1.0 1.0) (1.0 1.0))
      (const v0 0)
      (if-eqz v0 :left)
      (goto :right)

      (:left)
      (.src_block "LFoo;.bar:()V" 2 (1.0 1.0) (0.0 0.0) (0.0 0.0))
      (const v1 1)
      (goto :middle)

      (:right)
      (.src_block "LFoo;.bar:()V" 3 (0.0 0.0) (1.0 1.0) (0.0 0.0))
      (const v1 1)

      (:middle)
      (.src_block "LFoo;.bar:()V" 4 (1.0 1.0) (1.0 1.0) (0.0 0.0))
      (return-void)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));
  method->get_code()->build_cfg();

  dedup_blocks_impl::Config empty_config;
  dedup_blocks_impl::DedupBlocks db(&empty_config, method);
  db.run();
  method->get_code()->clear_cfg();

  const auto* expected_str = R"(
    (
      (.src_block "LFoo;.bar:()V" 1 (1.0 1.0) (1.0 1.0) (1.0 1.0))
      (const v0 0)
      (if-eqz v0 :left)

      (.src_block "LFoo;.bar:()V" 3 (0.0 0.0) (1.0 1.0) (0.0 0.0))

      (:middle)
      (.src_block "LFoo;.bar:()V" 4294967295 (1.0 1.0) (1.0 1.0) (0.0 0.0))
      (const v1 1)
      (.src_block "LFoo;.bar:()V" 4 (1.0 1.0) (1.0 1.0) (0.0 0.0))
      (return-void)

      (:left)
      (.src_block "LFoo;.bar:()V" 2 (1.0 1.0) (0.0 0.0) (0.0 0.0))
      (goto :middle)
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

TEST_F(SourceBlocksTest, dedup_multiple_interactions_in_same_block) {
  g_redex->instrument_mode = true;
  IRList::CONSECUTIVE_STYLE = IRList::ConsecutiveStyle::kChain;
  DexMethod* method = create_method("multiple_interactions");

  const auto* str = R"(
    (
      (.src_block "LFoo;.bar:()V" 1 (5.0 1.0) (5.0 1.0) (5.0 1.0))
      (const v0 0)
      (if-eqz v0 :left)
      (goto :right)

      (:left)
      (.src_block "LFoo;.bar:()V" 2 (2.0 0.5) (0.0 0.0) (0.0 0.0))
      (const v1 1)
      (.src_block "LFoo;.bar:()V" 2 (1.0 0.5) (0.0 0.0) (0.0 0.0))
      (const v2 2)
      (const v3 3)
      (goto :middle)

      (:right)
      (.src_block "LFoo;.bar:()V" 3 (0.0 0.0) (3.0 0.5) (0.0 0.0))
      (const v1 1)
      (.src_block "LFoo;.bar:()V" 3 (0.0 0.0) (2.0 0.4) (0.0 0.0))
      (const v2 2)
      (const v3 3)

      (:middle)
      (.src_block "LFoo;.bar:()V" 4 (5.0 0.5) (5.0 0.5) (0.0 0.0))
      (return-void)
    )
  )";

  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));
  method->get_code()->build_cfg();

  dedup_blocks_impl::Config empty_config;
  dedup_blocks_impl::DedupBlocks db(&empty_config, method);
  db.run();
  method->get_code()->clear_cfg();

  const auto* expected_str = R"(
    (
      (.src_block "LFoo;.bar:()V" 1 (5.0 1.0) (5.0 1.0) (5.0 1.0))
      (const v0 0)
      (if-eqz v0 :left)

      (.src_block "LFoo;.bar:()V" 3 (0.0 0.0) (3.0 0.5) (0.0 0.0))
      (const v1 1)
      (.src_block "LFoo;.bar:()V" 3 (0.0 0.0) (2.0 0.4) (0.0 0.0))

      (:synthetic)
      (.src_block "LFoo;.bar:()V" 4294967295 (1.0 0.5) (2.0 0.4) (0.0 0.0))
      (const v2 2)
      (const v3 3)
      (.src_block "LFoo;.bar:()V" 4 (5.0 0.5) (5.0 0.5) (0.0 0.0))
      (return-void)

      (:left)
      (.src_block "LFoo;.bar:()V" 2 (2.0 0.5) (0.0 0.0) (0.0 0.0))
      (const v1 1)
      (.src_block "LFoo;.bar:()V" 2 (1.0 0.5) (0.0 0.0) (0.0 0.0))
      (goto :synthetic)
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected_code.get(), method->get_code());
}

TEST_F(SourceBlocksTest, create_synth_sb_from_val) {
  g_redex->instrument_mode = true;
  IRList::CONSECUTIVE_STYLE = IRList::ConsecutiveStyle::kChain;
  auto* foo_method = create_method("LFoo");

  constexpr const char* kFoo = R"(
    (
      (.src_block "LFoo;.bar:()V" 0 (1.0 1.0) (0.0 1.0) (0.5 0.4))
      (.src_block "LFoo;.bar:()V" 1 (1.0 1.0) (0.0 1.0) (0.5 0.4))
      (const v0 0)
      (.src_block "LFoo;.bar:()V" 2 (1.0 1.0) (0.0 1.0) (0.5 0.4))
      (const v1 1)

      (.src_block "LFoo;.bar:()V" 3 (1.0 1.0) (0.0 1.0) (0.5 0.4))

      (return-void)
    )
  )";

  foo_method->set_code(assembler::ircode_from_string(kFoo));
  foo_method->get_code()->build_cfg();

  auto* bar_method = create_method("LBar");

  constexpr const char* kBar = R"(
    (
      (const v0 0)
      (const v1 1)
      (return-void)
    )
  )";

  bar_method->set_code(assembler::ircode_from_string(kBar));
  bar_method->get_code()->build_cfg();

  source_blocks::insert_synthetic_source_blocks_in_method(bar_method, [&]() {
    return clone_as_synthetic(
        source_blocks::get_first_source_block_of_method(foo_method), bar_method,
        SourceBlock::Val{1, 0});
  });

  EXPECT_EQ(get_blocks_as_txt(bar_method->get_code()->cfg().blocks()),
            R"(B0: LBar;.bar:()V@4294967295(1:0|1:0|1:0))");
}

TEST_F(SourceBlocksTest, create_synth_sb_from_opt_val) {
  g_redex->instrument_mode = true;
  IRList::CONSECUTIVE_STYLE = IRList::ConsecutiveStyle::kChain;
  auto* foo_method = create_method("LFoo");

  constexpr const char* kFoo = R"(
    (
      (.src_block "LFoo;.bar:()V" 0 (1.0 1.0) (0.0 1.0) (0.5 0.4))
      (.src_block "LFoo;.bar:()V" 1 (1.0 1.0) (0.0 1.0) (0.5 0.4))
      (const v0 0)
      (.src_block "LFoo;.bar:()V" 2 (1.0 1.0) (0.0 1.0) (0.5 0.4))
      (const v1 1)

      (.src_block "LFoo;.bar:()V" 3 (1.0 1.0) (0.0 1.0) (0.5 0.4))

      (return-void)
    )
  )";

  foo_method->set_code(assembler::ircode_from_string(kFoo));
  foo_method->get_code()->build_cfg();

  auto* bar_method = create_method("LBar");

  constexpr const char* kBar = R"(
    (
      (const v0 0)
      (const v1 1)
      (return-void)
    )
  )";

  bar_method->set_code(assembler::ircode_from_string(kBar));
  bar_method->get_code()->build_cfg();

  source_blocks::insert_synthetic_source_blocks_in_method(bar_method, [&]() {
    return clone_as_synthetic(
        source_blocks::get_first_source_block_of_method(foo_method),
        bar_method);
  });

  EXPECT_EQ(get_blocks_as_txt(bar_method->get_code()->cfg().blocks()),
            R"(B0: LBar;.bar:()V@4294967295(1:1|0:1|0.5:0.4))");
}

TEST_F(SourceBlocksTest, create_synth_sb_from_val_list) {
  g_redex->instrument_mode = true;
  IRList::CONSECUTIVE_STYLE = IRList::ConsecutiveStyle::kChain;
  auto* foo_method = create_method("LFoo");

  constexpr const char* kFoo = R"(
    (
      (.src_block "LFoo;.bar:()V" 0 (1.0 1.0) (0.0 1.0) (0.5 0.4))
      (.src_block "LFoo;.bar:()V" 1 (1.0 1.0) (0.0 1.0) (0.5 0.4))
      (const v0 0)
      (.src_block "LFoo;.bar:()V" 2 (1.0 1.0) (0.0 1.0) (0.5 0.4))
      (const v1 1)

      (.src_block "LFoo;.bar:()V" 3 (0.5 1.0) (0.0 1.0) (1.0 0.4))

      (return-void)
    )
  )";

  foo_method->set_code(assembler::ircode_from_string(kFoo));
  foo_method->get_code()->build_cfg();

  auto* bar_method = create_method("LBar");

  constexpr const char* kBar = R"(
    (
      (const v0 0)
      (const v1 1)
      (return-void)
    )
  )";

  bar_method->set_code(assembler::ircode_from_string(kBar));
  bar_method->get_code()->build_cfg();

  source_blocks::insert_synthetic_source_blocks_in_method(bar_method, [&]() {
    auto* first_sb =
        source_blocks::get_first_source_block_of_method(foo_method);
    auto* last_sb = source_blocks::get_last_source_block(
        foo_method->get_code()->cfg().entry_block());
    std::vector<SourceBlock*> vec = {first_sb, last_sb};
    return clone_as_synthetic(first_sb, foo_method, vec);
  });

  EXPECT_EQ(get_blocks_as_txt(bar_method->get_code()->cfg().blocks()),
            R"(B0: LFoo;.bar:()V@4294967295(1:1|0:1|1:0.4))");
}

TEST_F(SourceBlocksTest, metadata_indegrees_test) {
  auto* method = create_method();
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();

  ASSERT_EQ(cfg.num_blocks(), 1u);
  auto* b = cfg.blocks()[0];

  auto* b1 = cfg.create_block();
  auto* b2 = cfg.create_block();
  auto* b3 = cfg.create_block();
  auto* b4 = cfg.create_block();

  cfg.add_edge(b, b1, EDGE_GOTO);
  cfg.add_edge(b, b2, EDGE_BRANCH);
  cfg.add_edge(b1, b3, EDGE_GOTO);
  cfg.add_edge(b2, b3, EDGE_GOTO);
  cfg.add_edge(b1, b4, method->get_class(), 0);
  cfg.add_edge(b4, b3, EDGE_GOTO);

  auto profile = single_profile(
      "(0.1:0.5 g(0.2:0.4 g(0.3:0.3) t(0.4:0.2 g)) b(0.5:0.1 g))");

  auto res = insert_custom_source_blocks_get_indegrees(
      &method->get_deobfuscated_name(), &cfg, profile,
      /*serialize=*/true);

  UnorderedMap<Block*, uint32_t> expected_indegrees;
  expected_indegrees.emplace(b, 0);
  expected_indegrees.emplace(b1, 1);
  expected_indegrees.emplace(b2, 1);
  expected_indegrees.emplace(b3, 3);
  expected_indegrees.emplace(b4, 1);

  for (auto& entry : UnorderedIterable(expected_indegrees)) {
    Block* block = entry.first;
    uint32_t expected = entry.second;
    EXPECT_EQ(res.at(block), expected);
  }
}

TEST_F(SourceBlocksTest, source_block_val_equality) {
  auto sb1 =
      SourceBlock(DexString::make_string("blah"), 10, {SourceBlock::Val(1, 1)});
  auto sb2 =
      SourceBlock(DexString::make_string("blah"), 10, {SourceBlock::Val(1, 1)});
  ASSERT_EQ(sb1, sb2);
}

TEST_F(SourceBlocksTest, source_block_val_inequality) {
  auto sb1 = SourceBlock(DexString::make_string("blah"), 10,
                         {SourceBlock::Val(.1, 1)});
  auto sb2 =
      SourceBlock(DexString::make_string("blah"), 10, {SourceBlock::Val(1, 1)});
  ASSERT_NE(sb1, sb2);
}

TEST_F(SourceBlocksTest, source_block_appear_100_inequality) {
  auto sb1 = SourceBlock(DexString::make_string("blah"), 10,
                         {SourceBlock::Val(1, .1)});
  auto sb2 =
      SourceBlock(DexString::make_string("blah"), 10, {SourceBlock::Val(1, 1)});
  ASSERT_NE(sb1, sb2);
}

TEST_F(SourceBlocksTest, dedup_block_with_source_blocks_in_instrumentation) {

  g_redex->instrument_mode = true;

  auto* foo_method = create_method("LFoo");

  const auto* const kCode = R"(
    (
      ; A
      (const v0 0)
      (mul-int v0 v0 v0)
      (if-eqz v0 :D)

      (:C)
      (mul-int v0 v0 v0)
      (add-int v0 v0 v0)
      (invoke-static () "LFooX;.bar:()V")
      (move-result v1)
      (goto :E)

      (:D)
      (mul-int v0 v0 v0)
      (add-int v0 v0 v0)
      (invoke-static () "LFooX;.bar:()V")
      (move-result v1)
      (goto :E)

      (:E)
      (return-void)
    )
  )";

  foo_method->set_code(assembler::ircode_from_string(
      replace_all(kCode, "LFooX;", show(foo_method->get_class()))));

  foo_method->get_code()->build_cfg();

  auto res = source_blocks::insert_source_blocks(
      foo_method, &foo_method->get_code()->cfg(), {},
      /*serialize=*/true, true);

  // Set the source block ids so that two are the same
  auto blocks = foo_method->get_code()->cfg().blocks();
  ASSERT_EQ(blocks.size(), 4);
  auto block1_sbs = source_blocks::gather_source_blocks(blocks[1]);
  auto block2_sbs = source_blocks::gather_source_blocks(blocks[2]);
  ASSERT_EQ(block1_sbs.size(), 2);
  ASSERT_EQ(block2_sbs.size(), 2);
  auto* sb1 = block2_sbs[0];
  auto* sb2 = block2_sbs[1];
  sb1->id = 1;
  sb2->id = 2;

  dedup_blocks_impl::Config empty_config;
  dedup_blocks_impl::DedupBlocks db(&empty_config, foo_method);
  db.run();
  foo_method->get_code()->clear_cfg();

  foo_method->get_code()->build_cfg();

  auto post_dedup_blocks = foo_method->get_code()->cfg().blocks();
  ASSERT_EQ(post_dedup_blocks.size(), 2);
}

TEST_F(SourceBlocksTest,
       do_not_dedup_block_named_source_blocks_in_instrumentation) {

  g_redex->instrument_mode = true;

  auto* foo_method = create_method("LFoo");

  const auto* const kCode = R"(
    (
      ; A
      (const v0 0)
      (mul-int v0 v0 v0)
      (if-eqz v0 :D)

      (:C)
      (mul-int v0 v0 v0)
      (add-int v0 v0 v0)
      (invoke-static () "LFooX;.bar:()V")
      (move-result v1)
      (goto :E)

      (:D)
      (mul-int v0 v0 v0)
      (add-int v0 v0 v0)
      (invoke-static () "LFooX;.bar:()V")
      (move-result v1)
      (goto :E)

      (:E)
      (return-void)
    )
  )";

  foo_method->set_code(assembler::ircode_from_string(
      replace_all(kCode, "LFooX;", show(foo_method->get_class()))));

  foo_method->get_code()->build_cfg();

  auto res = source_blocks::insert_source_blocks(
      foo_method, &foo_method->get_code()->cfg(), {},
      /*serialize=*/true, true);

  // Set the source block ids so that two are the same
  auto blocks = foo_method->get_code()->cfg().blocks();
  ASSERT_EQ(blocks.size(), 4);
  auto block1_sbs = source_blocks::gather_source_blocks(blocks[1]);
  auto block2_sbs = source_blocks::gather_source_blocks(blocks[2]);
  ASSERT_EQ(block1_sbs.size(), 2);
  ASSERT_EQ(block2_sbs.size(), 2);
  // auto sb1 = block1_sbs[1];
  auto* sb1 = block2_sbs[0];
  auto* sb2 = block2_sbs[1];
  sb1->id = 1;
  sb2->id = 2;

  // Set the source block src so the origin method is different
  sb1->src = DexString::make_string("LFoo0;.baz:()V");
  sb2->src = DexString::make_string("LFoo0;.baz:()V");

  dedup_blocks_impl::Config empty_config;
  dedup_blocks_impl::DedupBlocks db(&empty_config, foo_method);
  db.run();
  foo_method->get_code()->clear_cfg();
  foo_method->get_code()->build_cfg();

  auto post_dedup_blocks = foo_method->get_code()->cfg().blocks();
  ASSERT_EQ(post_dedup_blocks.size(), 4);
}

TEST_F(SourceBlocksTest,
       do_not_dedup_block_chained_source_blocks_in_instrumentation) {

  g_redex->instrument_mode = true;

  auto* foo_method = create_method("LFoo");

  const auto* kCode = R"(
    (
      ; A
      (const v0 0)
      (mul-int v0 v0 v0)
      (if-eqz v0 :D)

      (:C)
      (mul-int v0 v0 v0)
      (add-int v0 v0 v0)
      (invoke-static () "LFooX;.bar:()V")
      (move-result v1)
      (goto :E)

      (:D)
      (mul-int v0 v0 v0)
      (add-int v0 v0 v0)
      (invoke-static () "LFooX;.bar:()V")
      (move-result v1)
      (goto :E)

      (:E)
      (return-void)
    )
  )";

  foo_method->set_code(assembler::ircode_from_string(
      replace_all(kCode, "LFooX;", show(foo_method->get_class()))));

  foo_method->get_code()->build_cfg();

  auto res = source_blocks::insert_source_blocks(
      foo_method, &foo_method->get_code()->cfg(), {},
      /*serialize=*/true, true);

  // Set the source block ids so that two are the same
  auto blocks = foo_method->get_code()->cfg().blocks();
  ASSERT_EQ(blocks.size(), 4);
  auto block1_sbs = source_blocks::gather_source_blocks(blocks[1]);
  auto block2_sbs = source_blocks::gather_source_blocks(blocks[2]);
  ASSERT_EQ(block1_sbs.size(), 2);
  ASSERT_EQ(block2_sbs.size(), 2);
  auto* sb1 = block2_sbs[0];
  auto* sb2 = block2_sbs[1];
  sb1->id = 1;
  sb2->id = 2;

  // Add chained source blocks
  sb1->next = std::make_unique<SourceBlock>(
      SourceBlock(foo_method->get_name(), 10, {}));
  sb2->next = std::make_unique<SourceBlock>(
      SourceBlock(foo_method->get_name(), 11, {}));

  dedup_blocks_impl::Config empty_config;
  dedup_blocks_impl::DedupBlocks db(&empty_config, foo_method);
  db.run();
  foo_method->get_code()->clear_cfg();
  foo_method->get_code()->build_cfg();

  auto post_dedup_blocks = foo_method->get_code()->cfg().blocks();
  ASSERT_EQ(post_dedup_blocks.size(), 4);
}

TEST_F(SourceBlocksTest,
       do_not_dedup_tail_chained_source_blocks_in_instrumentation) {

  g_redex->instrument_mode = true;

  auto* foo_method = create_method("LFoo");

  const auto* const kCode = R"(
    (
      ; A
      (const v0 0)
      (mul-int v0 v0 v0)
      (if-eqz v0 :D)

      (:C)
      (mul-int v0 v0 v0)
      (add-int v0 v0 v0)
      (invoke-static () "LFooX;.bar:()V")
      (move-result v1)
      (goto :E)

      (:D)
      (const v1 1)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (invoke-static () "LFooX;.bar:()V")
      (move-result v1)
      (goto :E)

      (:E)
      (return-void)
    )
  )";

  foo_method->set_code(assembler::ircode_from_string(
      replace_all(kCode, "LFooX;", show(foo_method->get_class()))));

  foo_method->get_code()->build_cfg();

  auto res = source_blocks::insert_source_blocks(
      foo_method, &foo_method->get_code()->cfg(), {},
      /*serialize=*/true, true);

  // Set the source block ids so that two are the same
  auto blocks = foo_method->get_code()->cfg().blocks();
  ASSERT_EQ(blocks.size(), 4);
  auto block1_sbs = source_blocks::gather_source_blocks(blocks[1]);
  auto block2_sbs = source_blocks::gather_source_blocks(blocks[2]);
  ASSERT_EQ(block1_sbs.size(), 2);
  ASSERT_EQ(block2_sbs.size(), 2);
  auto* sb1 = block1_sbs[1];
  auto* sb2 = block2_sbs[1];
  sb2->id = 2;

  // Add a chained source block
  sb1->next = std::make_unique<SourceBlock>(
      SourceBlock(foo_method->get_name(), 10, {}));
  sb2->next = std::make_unique<SourceBlock>(
      SourceBlock(foo_method->get_name(), 11, {}));

  dedup_blocks_impl::Config empty_config;
  dedup_blocks_impl::DedupBlocks db(&empty_config, foo_method);
  db.run();
  foo_method->get_code()->clear_cfg();
  foo_method->get_code()->build_cfg();

  auto post_dedup_blocks = foo_method->get_code()->cfg().blocks();
  ASSERT_EQ(post_dedup_blocks.size(), 4);
  std::unordered_set<uint32_t> seen_ids;
  for (auto* block : post_dedup_blocks) {
    auto source_blocks = source_blocks::gather_source_blocks(block);
    for (auto* source_block : source_blocks) {
      seen_ids.emplace(source_block->id);
    }
  }
  ASSERT_TRUE(seen_ids.count(11));
  ASSERT_TRUE(seen_ids.count(10));
}

TEST_F(SourceBlocksTest, source_block_add) {
  const auto* s = DexString::make_string("x");
  SourceBlock a(s, 0, std::vector<SourceBlock::Val>{SourceBlock::Val(10, 30)});
  SourceBlock b(s, 0, std::vector<SourceBlock::Val>{SourceBlock::Val(5, 70)});
  a.add(b);
  ASSERT_TRUE(a.get_val(0).has_value());
  EXPECT_FLOAT_EQ(*a.get_val(0), 15.0f); // val sums
  EXPECT_FLOAT_EQ(*a.get_appear100(0), 70.0f); // appear100 maxes, never sums

  // Adding into a "none" val copies the other value in (matches max()'s
  // NaN-aware behavior).
  SourceBlock none(s, 0,
                   std::vector<SourceBlock::Val>{SourceBlock::Val::none()});
  none.add(b);
  ASSERT_TRUE(none.get_val(0).has_value());
  EXPECT_FLOAT_EQ(*none.get_val(0), 5.0f);
  EXPECT_FLOAT_EQ(*none.get_appear100(0), 70.0f);
}

TEST_F(SourceBlocksTest, source_block_add_none_other_leaves_receiver) {
  // The NaN-aware behavior is symmetric: adding a "none" other must leave the
  // receiver untouched (not zero it, not turn it into none).
  const auto* s = DexString::make_string("x");
  SourceBlock a(s, 0, std::vector<SourceBlock::Val>{SourceBlock::Val(5, 30)});
  SourceBlock none(s, 0,
                   std::vector<SourceBlock::Val>{SourceBlock::Val::none()});
  a.add(none);
  ASSERT_TRUE(a.get_val(0).has_value());
  EXPECT_FLOAT_EQ(*a.get_val(0), 5.0f);
  EXPECT_FLOAT_EQ(*a.get_appear100(0), 30.0f);
}

TEST_F(SourceBlocksTest, source_block_add_multi_interaction) {
  // add() combines every interaction slot independently: vals sum, appear100
  // maxes, per index.
  const auto* s = DexString::make_string("x");
  SourceBlock a(s, 0,
                std::vector<SourceBlock::Val>{SourceBlock::Val(1, 10),
                                              SourceBlock::Val(2, 20)});
  SourceBlock b(s, 0,
                std::vector<SourceBlock::Val>{SourceBlock::Val(3, 40),
                                              SourceBlock::Val(4, 5)});
  a.add(b);
  EXPECT_FLOAT_EQ(*a.get_val(0), 4.0f); // 1 + 3
  EXPECT_FLOAT_EQ(*a.get_appear100(0), 40.0f); // max(10, 40)
  EXPECT_FLOAT_EQ(*a.get_val(1), 6.0f); // 2 + 4
  EXPECT_FLOAT_EQ(*a.get_appear100(1), 20.0f); // max(20, 5)
}

// Golden for the N:1 outline root: clone_as_synthetic_summing SUMS `val` across
// the inputs (an outlined body runs about the sum of its call sites' counts)
// and maxes appear100 -- unlike the plain clone_as_synthetic(many), which maxes
// val.
TEST_F(SourceBlocksTest, clone_as_synthetic_summing) {
  const auto* s = DexString::make_string("x");
  SourceBlock s1(s, 7, std::vector<SourceBlock::Val>{SourceBlock::Val(10, 5)});
  SourceBlock s2(s, 8, std::vector<SourceBlock::Val>{SourceBlock::Val(20, 90)});
  SourceBlock s3(s, 9, std::vector<SourceBlock::Val>{SourceBlock::Val(30, 10)});
  std::vector<SourceBlock*> many{&s1, &s2, &s3};

  auto summed = source_blocks::clone_as_synthetic_summing(&s1, nullptr, many);
  ASSERT_TRUE(summed->get_val(0).has_value());
  EXPECT_FLOAT_EQ(*summed->get_val(0), 60.0f); // 10 + 20 + 30 (SUM)
  EXPECT_FLOAT_EQ(*summed->get_appear100(0), 90.0f); // max(5, 90, 10)
  EXPECT_EQ(summed->id, SourceBlock::kSyntheticId);

  // Contrast: the plain (max) clone maxes val -- would undercount an outline.
  auto maxed = source_blocks::clone_as_synthetic(&s1, nullptr, many);
  EXPECT_FLOAT_EQ(*maxed->get_val(0), 30.0f); // max, not 60
}
TEST_F(SourceBlocksTest, violation_name_round_trip) {
  for (size_t i = 0;
       i != static_cast<size_t>(ViolationsHelper::Violation::ViolationSize);
       ++i) {
    auto v = static_cast<ViolationsHelper::Violation>(i);
    auto name = get_violation_name(v);
    SCOPED_TRACE(std::string(name));
    EXPECT_EQ(violation_name_to_enum(name), v);
  }
}

TEST_F(SourceBlocksTest, violation_name_to_enum_rejects_unknown_names) {
  EXPECT_EQ(violation_name_to_enum("NotAViolation"), std::nullopt);
  // Matching is exact, so a plausible-looking alternative spelling has to be
  // rejected rather than silently tracking nothing.
  EXPECT_EQ(violation_name_to_enum("chain_and_dom"), std::nullopt);
  EXPECT_EQ(violation_name_to_enum(""), std::nullopt);
}

// Reporting no top-N list at all is a reasonable thing to configure. Before
// `top_n` became configurable it was the literal 10, so the ranking code never
// saw an empty list; now `top_n: 0` reaches `back()` on one.
TEST_F(SourceBlocksTest, violations_helper_tolerates_zero_top_n) {
  constexpr const char* kCode = R"(
    (
      (.src_block "LZero;.bar:()V" 0 (1.0 1.0))
      (const v0 0)
      (.src_block "LZero;.bar:()V" 1 (1.0 1.0))
      (return-void)
    )
  )";

  auto* method = create_method("LZeroTopN");
  ASSERT_NE(method, nullptr);
  // @lint-ignore NULLSAFECLANG (guarded by ASSERT_NE above)
  method->set_code(assembler::ircode_from_string(kCode));
  // @lint-ignore NULLSAFECLANG (guarded by ASSERT_NE above)
  auto* code = method->get_code();
  ASSERT_NE(code, nullptr);
  // @lint-ignore NULLSAFECLANG (guarded by ASSERT_NE above)
  ::Scope scope{type_class(method->get_class())};

  ViolationsHelper vh(ViolationsHelper::Violation::kUncoveredSourceBlocks,
                      scope,
                      /*top_n=*/0,
                      /*to_vis=*/{},
                      /*track_intermethod_violations=*/false,
                      /*print_all_violations=*/false,
                      /*ignore_undefined=*/false);

  code->build_cfg();
  strip_source_blocks(code->cfg());
  ASSERT_GT(
      compute(ViolationsHelper::Violation::kUncoveredSourceBlocks, code->cfg()),
      0u);
  code->clear_cfg();

  // The regressed method has nowhere to be ranked. Without the guard this
  // dereferences back() on an empty vector, whose data() is null.
  vh.process(nullptr);
}
namespace {

// A MetricsSink that is not backed by a PassManager, which is the point: after
// the sink was abstracted, violations reporting no longer needs one.
class RecordingSink : public MetricsSink {
 public:
  std::optional<int64_t> get(const std::string& key) const {
    for (const auto& [k, v] : metrics) {
      if (k == key) {
        return v;
      }
    }
    return std::nullopt;
  }

  std::vector<std::pair<std::string, int64_t>> metrics;

 protected:
  void report_metric(const std::string& key, int64_t value) override {
    metrics.emplace_back(key, value);
  }
};

constexpr const char* kBranchWithSourceBlocks = R"(
    (
      (.src_block "LFoo;.bar:()V" 0 (1.0 1.0))
      (const v0 0)
      (if-eqz v0 :true)

      (.src_block "LFoo;.bar:()V" 1 (1.0 1.0))
      (const v1 1)
      (goto :end)

      (:true)
      (.src_block "LFoo;.bar:()V" 2 (1.0 1.0))
      (const v1 2)

      (:end)
      (.src_block "LFoo;.bar:()V" 3 (1.0 1.0))
      (return-void)
    )
  )";

} // namespace

TEST_F(SourceBlocksTest, violations_helper_reports_to_metrics_sink) {
  auto* method = create_method("LFoo");
  ASSERT_NE(method, nullptr);
  // @lint-ignore NULLSAFECLANG (guarded by ASSERT_NE above)
  method->set_code(assembler::ircode_from_string(kBranchWithSourceBlocks));
  // @lint-ignore NULLSAFECLANG (guarded by ASSERT_NE above)
  auto* code = method->get_code();
  ASSERT_NE(code, nullptr);

  // @lint-ignore NULLSAFECLANG (guarded by ASSERT_NE above)
  ::Scope scope{type_class(method->get_class())};
  // Every block carries a source block, so the baseline is zero uncovered
  // blocks.
  ViolationsHelper vh(ViolationsHelper::Violation::kUncoveredSourceBlocks,
                      scope,
                      /*top_n=*/10,
                      /*to_vis=*/{},
                      /*track_intermethod_violations=*/false,
                      /*print_all_violations=*/false,
                      /*ignore_undefined=*/false);

  code->build_cfg();
  strip_source_blocks(code->cfg());
  auto expected_violations = static_cast<int64_t>(compute(
      ViolationsHelper::Violation::kUncoveredSourceBlocks, code->cfg()));
  code->clear_cfg();
  ASSERT_GT(expected_violations, 0);

  RecordingSink sink;
  vh.process(&sink);

  EXPECT_EQ(sink.get("new_violations"), expected_violations);
  EXPECT_EQ(sink.get("new_method_violations"), 0);
  // The worst offender is reported under a nested scope, which exercises the
  // sink's key assembly.
  auto name = show(method);
  EXPECT_EQ(sink.get("top_changes.0.delta." + name), expected_violations);
  EXPECT_TRUE(sink.get("top_changes.0.size." + name).has_value());
}

TEST_F(SourceBlocksTest, violations_helper_reports_nothing_without_a_sink) {
  auto* method = create_method("LFoo");
  ASSERT_NE(method, nullptr);
  // @lint-ignore NULLSAFECLANG (guarded by ASSERT_NE above)
  method->set_code(assembler::ircode_from_string(kBranchWithSourceBlocks));
  // @lint-ignore NULLSAFECLANG (guarded by ASSERT_NE above)
  auto* code = method->get_code();
  ASSERT_NE(code, nullptr);

  // @lint-ignore NULLSAFECLANG (guarded by ASSERT_NE above)
  ::Scope scope{type_class(method->get_class())};
  ViolationsHelper vh(ViolationsHelper::Violation::kUncoveredSourceBlocks,
                      scope,
                      /*top_n=*/10,
                      /*to_vis=*/{},
                      /*track_intermethod_violations=*/false,
                      /*print_all_violations=*/false,
                      /*ignore_undefined=*/false);

  code->build_cfg();
  strip_source_blocks(code->cfg());
  code->clear_cfg();

  // The nullable contract the destructor relies on: no sink, no crash, and the
  // subsequent destructor must not report a second time either.
  vh.process(nullptr);
}
