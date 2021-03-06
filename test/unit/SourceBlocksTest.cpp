/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SourceBlocks.h"

#include <atomic>
#include <sstream>

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "Creators.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "RedexTest.h"
#include "Show.h"

using namespace cfg;
using namespace source_blocks;

class SourceBlocksTest : public RedexTest {
 public:
  static DexMethod* create_method() {
    // Create a totally new class.
    size_t c = s_counter.fetch_add(1);
    std::string name = std::string("LFoo") + std::to_string(c) + ";";
    ClassCreator cc{DexType::make_type(name.c_str())};
    cc.set_super(type::java_lang_Object());

    constexpr const char* kCode = "((return-void))";

    // Empty code isn't really legal. But it does not matter for us.
    auto m = DexMethod::make_method(name + ".bar:()V")
                 ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                                 assembler::ircode_from_string(kCode), false);
    cc.add_method(m);

    cc.create();

    return m;
  }

  static std::string get_blocks_as_txt(std::initializer_list<Block*> l) {
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
        oss << " " << show(sb->src) << "@" << sb->id << "(" << sb->val << ")";
      }
    }
    return remove_count(oss.str());
  }

  static std::string remove_count(const std::string& str) {
    const std::string needle = "LFoo";
    std::string res = str;
    size_t i = 0;
    for (;;) {
      i = res.find(needle, i);
      if (i == std::string::npos) {
        break;
      }
      i += needle.size();
      size_t j = i;
      while (j < res.size() && res[j] != ';') {
        ++j;
      }
      if (j == res.size()) {
        break;
      }
      if (i == j) {
        continue;
      }
      res.replace(i, j - i, "");
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

 private:
  static std::atomic<size_t> s_counter;
};
std::atomic<size_t> SourceBlocksTest::s_counter{0};

TEST_F(SourceBlocksTest, minimal_serialize) {
  auto method = create_method();
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();

  ASSERT_EQ(cfg.blocks().size(), 1u);

  auto res = insert_source_blocks(method, &cfg, /*profile=*/nullptr,
                                  /*serialize=*/true);

  EXPECT_EQ(res.block_count, 1u);
  EXPECT_EQ(res.serialized, "(0)");
}

TEST_F(SourceBlocksTest, complex_serialize) {
  auto method = create_method();
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();

  ASSERT_EQ(cfg.blocks().size(), 1u);
  auto b = cfg.blocks()[0];

  // We're gonna just focus on blocks and edges, no instruction constraints.
  auto b1 = cfg.create_block();
  auto b2 = cfg.create_block();
  auto b3 = cfg.create_block();
  auto b4 = cfg.create_block();

  cfg.add_edge(b, b1, EDGE_GOTO);
  cfg.add_edge(b, b2, EDGE_BRANCH);
  cfg.add_edge(b1, b3, EDGE_GOTO);
  cfg.add_edge(b2, b3, EDGE_GOTO);
  cfg.add_edge(b1, b4, method->get_class(), 0);
  cfg.add_edge(b4, b3, EDGE_GOTO);

  auto res = insert_source_blocks(method, &cfg, /*profile=*/nullptr,
                                  /*serialize=*/true);

  EXPECT_EQ(res.block_count, 5u);
  EXPECT_EQ(res.serialized, "(0 g(1 g(2) t(3 g)) b(4 g))");
  EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), R"(B0: LFoo;.bar:()V@0(0)
B1: LFoo;.bar:()V@1(0)
B2: LFoo;.bar:()V@4(0)
B3: LFoo;.bar:()V@2(0)
B4: LFoo;.bar:()V@3(0))");
}

TEST_F(SourceBlocksTest, complex_deserialize) {
  auto method = create_method();
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();

  ASSERT_EQ(cfg.blocks().size(), 1u);
  auto b = cfg.blocks()[0];

  // We're gonna just focus on blocks and edges, no instruction constraints.
  auto b1 = cfg.create_block();
  auto b2 = cfg.create_block();
  auto b3 = cfg.create_block();
  auto b4 = cfg.create_block();

  cfg.add_edge(b, b1, EDGE_GOTO);
  cfg.add_edge(b, b2, EDGE_BRANCH);
  cfg.add_edge(b1, b3, EDGE_GOTO);
  cfg.add_edge(b2, b3, EDGE_GOTO);
  cfg.add_edge(b1, b4, method->get_class(), 0);
  cfg.add_edge(b4, b3, EDGE_GOTO);

  std::string profile = "(0.1 g(0.2 g(0.3) t(0.4 g)) b(0.5 g))";

  auto res = insert_source_blocks(method, &cfg, &profile,
                                  /*serialize=*/true);

  EXPECT_EQ(res.block_count, 5u);
  EXPECT_EQ(res.serialized, "(0 g(1 g(2) t(3 g)) b(4 g))");
  EXPECT_TRUE(res.profile_success);
  EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), R"(B0: LFoo;.bar:()V@0(0.1)
B1: LFoo;.bar:()V@1(0.2)
B2: LFoo;.bar:()V@4(0.5)
B3: LFoo;.bar:()V@2(0.3)
B4: LFoo;.bar:()V@3(0.4))");
}

TEST_F(SourceBlocksTest, complex_deserialize_failure) {
  auto method = create_method();
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();

  ASSERT_EQ(cfg.blocks().size(), 1u);
  auto b = cfg.blocks()[0];

  // We're gonna just focus on blocks and edges, no instruction constraints.
  auto b1 = cfg.create_block();
  auto b2 = cfg.create_block();
  auto b3 = cfg.create_block();
  auto b4 = cfg.create_block();

  cfg.add_edge(b, b1, EDGE_GOTO);
  cfg.add_edge(b, b2, EDGE_BRANCH);
  cfg.add_edge(b1, b3, EDGE_GOTO);
  cfg.add_edge(b2, b3, EDGE_GOTO);
  cfg.add_edge(b1, b4, method->get_class(), 0);
  cfg.add_edge(b4, b3, EDGE_GOTO);

  // Change the profiles a bit so they should not match.
  {
    std::string profile = "(0.1 b(0.2 g(0.3) t(0.4 g)) b(0.5 g))";
    auto res = insert_source_blocks(method, &cfg, &profile,
                                    /*serialize=*/true);
    EXPECT_FALSE(res.profile_success);
    EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), R"(B0: LFoo;.bar:()V@0(0)
B1: LFoo;.bar:()V@1(0)
B2: LFoo;.bar:()V@4(0)
B3: LFoo;.bar:()V@2(0)
B4: LFoo;.bar:()V@3(0))");
    strip_source_blocks(cfg);
  }

  {
    std::string profile = "(0.1 g(0.2 t(0.3) t(0.4 g)) b(0.5 g))";
    auto res = insert_source_blocks(method, &cfg, &profile,
                                    /*serialize=*/true);
    EXPECT_FALSE(res.profile_success);
    EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), R"(B0: LFoo;.bar:()V@0(0)
B1: LFoo;.bar:()V@1(0)
B2: LFoo;.bar:()V@4(0)
B3: LFoo;.bar:()V@2(0)
B4: LFoo;.bar:()V@3(0))");
    strip_source_blocks(cfg);
  }

  {
    std::string profile = "(0.1 g(0.2 g(0.3)) b(0.5 g))";
    auto res = insert_source_blocks(method, &cfg, &profile,
                                    /*serialize=*/true);
    EXPECT_FALSE(res.profile_success);
    EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), R"(B0: LFoo;.bar:()V@0(0)
B1: LFoo;.bar:()V@1(0)
B2: LFoo;.bar:()V@4(0)
B3: LFoo;.bar:()V@2(0)
B4: LFoo;.bar:()V@3(0))");
    strip_source_blocks(cfg);
  }

  {
    std::string profile = "(0.1 g(0.2 g(0.3) t(0.4 g)))";
    auto res = insert_source_blocks(method, &cfg, &profile,
                                    /*serialize=*/true);
    EXPECT_FALSE(res.profile_success);
    EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), R"(B0: LFoo;.bar:()V@0(0)
B1: LFoo;.bar:()V@1(0)
B2: LFoo;.bar:()V@4(0)
B3: LFoo;.bar:()V@2(0)
B4: LFoo;.bar:()V@3(0))");
    strip_source_blocks(cfg);
  }
}
