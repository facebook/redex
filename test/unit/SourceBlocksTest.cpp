/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SourceBlocks.h"

#include <atomic>
#include <regex>
#include <sstream>

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "Creators.h"
#include "DexClass.h"
#include "IRAssembler.h"
#include "Inliner.h"
#include "RedexContext.h"
#include "RedexTest.h"
#include "Show.h"

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
    return std::vector<ProfileData>{std::make_pair(p, boost::none)};
  }

 private:
  static std::atomic<size_t> s_counter;
};
std::atomic<size_t> SourceBlocksTest::s_counter{0};

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
