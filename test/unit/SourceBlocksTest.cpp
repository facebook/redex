/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
#include "DexUtil.h"
#include "IRAssembler.h"
#include "Inliner.h"
#include "InlinerConfig.h"
#include "RedexTest.h"
#include "Show.h"

using namespace cfg;
using namespace source_blocks;

class SourceBlocksTest : public RedexTest {
 public:
  static DexMethod* create_method(const std::string& class_name = "LFoo",
                                  const std::string& code = "((return-void))") {
    // Create a totally new class.
    size_t c = s_counter.fetch_add(1);
    std::string name = class_name + std::to_string(c) + ";";
    ClassCreator cc{DexType::make_type(name.c_str())};
    cc.set_super(type::java_lang_Object());

    // Empty code isn't really legal. But it does not matter for us.
    auto m = DexMethod::make_method(name + ".bar:()V")
                 ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                                 assembler::ircode_from_string(code), false);
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
        if (sb->val) {
          oss << "(" << sb->val->val << ":" << sb->val->appear100 << ")";
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
  EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), R"(B0: LFoo;.bar:()V@0
B1: LFoo;.bar:()V@1
B2: LFoo;.bar:()V@4
B3: LFoo;.bar:()V@2
B4: LFoo;.bar:()V@3)");
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

  std::string profile =
      "(0.1:0.5 g(0.2:0.4 g(0.3:0.3) t(0.4:0.2 g)) b(0.5:0.1 g))";

  auto res = insert_source_blocks(method, &cfg, &profile,
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

  const std::string kSerializedExp = R"(B0: LFoo;.bar:()V@0
B1: LFoo;.bar:()V@1
B2: LFoo;.bar:()V@4
B3: LFoo;.bar:()V@2
B4: LFoo;.bar:()V@3)";

  // Change the profiles a bit so they should not match.
  {
    std::string profile =
        "(0.1:0.0 b(0.2:0.0 g(0.3:0.0) t(0.4:0.0 g)) b(0.5:0.0 g))";
    auto res = insert_source_blocks(method, &cfg, &profile,
                                    /*serialize=*/true);
    EXPECT_FALSE(res.profile_success);
    EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), kSerializedExp);
    strip_source_blocks(cfg);
  }

  {
    std::string profile =
        "(0.1:0.0 g(0.2:0.0 t(0.3:0.0) t(0.4:0.0 g)) b(0.5:0.0 g))";
    auto res = insert_source_blocks(method, &cfg, &profile,
                                    /*serialize=*/true);
    EXPECT_FALSE(res.profile_success);
    EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), kSerializedExp);
    strip_source_blocks(cfg);
  }

  {
    std::string profile = "(0.1:0.0 g(0.2:0.0 g(0.3:0.0)) b(0.5:0.0 g))";
    auto res = insert_source_blocks(method, &cfg, &profile,
                                    /*serialize=*/true);
    EXPECT_FALSE(res.profile_success);
    EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), kSerializedExp);
    strip_source_blocks(cfg);
  }

  {
    std::string profile = "(0.1:0.0 g(0.2:0.0 g(0.3:0.0) t(0.4:0.0 g)))";
    auto res = insert_source_blocks(method, &cfg, &profile,
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
    std::string profile = "(hello:world g(0.2 g(0.3) t(0.4 g)))";
    try {
      insert_source_blocks(method, &cfg, &profile, /*serialize=*/true);
      ADD_FAILURE() << "Expected exception.";
    } catch (const std::exception& e) {
      // This is not ours, but from std::stof. Message is not well-specified, I
      // think.
    }
  }
  // Not fully parseable as float (and not 'x').
  {
    std::string profile = "(0hello:world g(0.2 g(0.3) t(0.4 g)))";
    try {
      insert_source_blocks(method, &cfg, &profile, /*serialize=*/true);
      ADD_FAILURE() << "Expected exception.";
    } catch (const std::exception& e) {
      EXPECT_EQ(extract_exc_msg(e),
                "Did not find separating ':' in 0hello:world");
    }
  }
  // Missing appear100.
  {
    std::string profile = "(0.1 g(0.2 g(0.3) t(0.4 g)))";
    try {
      insert_source_blocks(method, &cfg, &profile, /*serialize=*/true);
      ADD_FAILURE() << "Expected exception.";
    } catch (const std::exception& e) {
      EXPECT_EQ(extract_exc_msg(e), "Could not find separator of 0.1");
    }
  }
  // Wrong character.
  {
    std::string profile = "(0.1/0.0 g(0.2/0 g(0.3/0) t(0.4/0 g)))";
    try {
      insert_source_blocks(method, &cfg, &profile, /*serialize=*/true);
      ADD_FAILURE() << "Expected exception.";
    } catch (const std::exception& e) {
      EXPECT_EQ(extract_exc_msg(e), "Did not find separating ':' in 0.1/0.0");
    }
  }
  // Not a float in appear.
  {
    std::string profile = "(0:0world g(0.2 g(0.3) t(0.4 g)))";
    try {
      insert_source_blocks(method, &cfg, &profile, /*serialize=*/true);
      ADD_FAILURE() << "Expected exception.";
    } catch (const std::exception& e) {
      EXPECT_EQ(extract_exc_msg(e),
                "Could not parse second part of 0:0world as float");
    }
  }
}

TEST_F(SourceBlocksTest, inline_normalization) {
  auto foo_method = create_method("LFoo");
  auto bar_method = create_method("LBar");

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
  std::string foo_profile = "(1.0:0.1 g(0.6:0.2) b(0.5:0.3 g))";
  auto res = insert_source_blocks(foo_method, &foo_cfg, &foo_profile,
                                  /*serialize=*/true);
  EXPECT_TRUE(res.profile_success);

  bar_method->set_code(assembler::ircode_from_string(
      replace_all(kCode, "LBarX;", show(bar_method->get_class()))));

  bar_method->get_code()->build_cfg();
  auto& bar_cfg = bar_method->get_code()->cfg();
  std::string bar_profile = "(1:0.1 g(0.4:0.2) b(0.2:0.3 g))";
  auto bar_res = insert_source_blocks(bar_method, &bar_cfg, &bar_profile,
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
  inliner::inline_with_cfg(foo_method, bar_method, invoke_insn, 1);

  // Values of LBar; should be halved.

  EXPECT_EQ(get_blocks_as_txt(foo_cfg.blocks()), R"(B0: LFoo;.bar:()V@0(1:0.1)
B2: LFoo;.bar:()V@2(0.5:0.3)
B3: LFoo;.bar:()V@1(0.6:0.2)
B4: LBar;.bar:()V@0(0.5:0.1)
B5: LBar;.bar:()V@2(0.1:0.3)
B6: LBar;.bar:()V@1(0.2:0.2))");
}

TEST_F(SourceBlocksTest, serialize_exc_injected) {
  auto foo_method = create_method("LFoo");

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
      insert_source_blocks(foo_method, &foo_cfg, /*profile=*/nullptr,
                           /*serialize=*/true, /*insert_after_excs=*/true);
  EXPECT_EQ(res.serialized, "(0(1)(2)(3) g(4) b(5 g))");
  EXPECT_EQ(
      get_blocks_as_txt(foo_cfg.blocks()),
      R"(B0: LFoo;.bar:()V@0 LFoo;.bar:()V@1 LFoo;.bar:()V@2 LFoo;.bar:()V@3
B2: LFoo;.bar:()V@5
B3: LFoo;.bar:()V@4)");
}

TEST_F(SourceBlocksTest, deserialize_exc_injected) {
  auto foo_method = create_method("LFoo");

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
  std::string profile = "(1:0(2:0)(3:0)(4:0) g(5:0) b(6:0 g))";
  auto res =
      insert_source_blocks(foo_method, &foo_cfg, &profile,
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

  std::string profile = "(0.1:0.1 g(x g(x) t(0.4:0.2 g)) b(x g))";

  auto res = insert_source_blocks(method, &cfg, &profile,
                                  /*serialize=*/true);

  EXPECT_EQ(res.block_count, 5u);
  EXPECT_EQ(res.serialized, "(0 g(1 g(2) t(3 g)) b(4 g))");
  EXPECT_TRUE(res.profile_success);
  EXPECT_EQ(get_blocks_as_txt({b, b1, b2, b3, b4}), R"(B0: LFoo;.bar:()V@0(0.1:0.1)
B1: LFoo;.bar:()V@1
B2: LFoo;.bar:()V@4
B3: LFoo;.bar:()V@2
B4: LFoo;.bar:()V@3(0.4:0.2))");
}
