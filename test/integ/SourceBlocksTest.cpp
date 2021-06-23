/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InsertSourceBlocks.h"

#include <algorithm>
#include <unordered_set>

#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "Inliner.h"
#include "InlinerConfig.h"
#include "RedexTest.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "SourceBlocks.h"

class SourceBlocksTest : public RedexIntegrationTest {
 protected:
  void enable_pass(InsertSourceBlocksPass& isbp) { isbp.m_force_run = true; }
  void enable_always_inject(InsertSourceBlocksPass& isbp) {
    isbp.m_always_inject = true;
  }
  void disable_always_inject(InsertSourceBlocksPass& isbp) {
    isbp.m_always_inject = false;
  }
  void set_insert_after_excs(InsertSourceBlocksPass& isbp, bool val) {
    isbp.m_insert_after_excs = val;
  }
  void set_profile(InsertSourceBlocksPass& isbp, std::string&& val) {
    isbp.m_profile_files = val;
  }
  void set_force_serialize(InsertSourceBlocksPass& isbp) {
    isbp.m_force_serialize = true;
  }

  std::string get_blocks_as_txt(const cfg::ControlFlowGraph& cfg) {
    std::ostringstream oss;
    bool first = true;
    for (auto* block : cfg.blocks()) {
      if (first) {
        first = false;
      } else {
        oss << "\n";
      }
      oss << "B" << block->id() << ":";
      auto vec = source_blocks::gather_source_blocks(block);
      for (auto* sb : vec) {
        oss << " " << sb->id;
        if (!sb->vals.empty()) {
          oss << "(";
          bool first_val = true;
          for (const auto& val : sb->vals) {
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
    return oss.str();
  }
};

TEST_F(SourceBlocksTest, source_blocks) {
  auto type = DexType::get_type("Lcom/facebook/redextest/SourceBlocksTest;");
  ASSERT_NE(type, nullptr);
  auto cls = type_class(type);
  ASSERT_NE(cls, nullptr);

  // Check that no code has source blocks so far.
  {
    for (const auto* m : cls->get_all_methods()) {
      if (m->get_code() == nullptr) {
        continue;
      }
      for (const auto& mie : *m->get_code()) {
        ASSERT_NE(mie.type, MFLOW_SOURCE_BLOCK);
      }
    }
  }

  // Run the pass, check that each block has a SourceBlock.
  {
    InsertSourceBlocksPass isbp{};
    run_passes({&isbp}, nullptr, Json::nullValue, [&](const auto&) {
      enable_pass(isbp);
      enable_always_inject(isbp);
      set_insert_after_excs(isbp, false);
    });

    for (auto* m : cls->get_all_methods()) {
      if (m->get_code() == nullptr) {
        continue;
      }
      cfg::ScopedCFG cfg{m->get_code()};
      std::unordered_set<uint32_t> seen_ids;
      for (const auto* b : cfg->blocks()) {
        bool seen_source_block_in_b{false};
        for (const auto& mie : *b) {
          if (mie.type != MFLOW_SOURCE_BLOCK) {
            continue;
          }

          EXPECT_FALSE(seen_source_block_in_b);
          seen_source_block_in_b = true;

          ASSERT_TRUE(mie.src_block != nullptr);

          EXPECT_EQ(seen_ids.count(mie.src_block->id), 0u);
          seen_ids.insert(mie.src_block->id);

          EXPECT_EQ(mie.src_block->src, m);
        }
        EXPECT_TRUE(seen_source_block_in_b);
      }
    }
  }

  // Run inliner, check that we have mix now.
  {
    inliner::InlinerConfig conf{};
    conf.use_cfg_inliner = true;
    auto scope = build_class_scope(stores);
    conf.populate(scope);

    ConcurrentMethodRefCache m_concurrent_resolved_refs;
    auto concurrent_resolver = [&](DexMethodRef* method, MethodSearch search) {
      return resolve_method(method, search, m_concurrent_resolved_refs);
    };

    auto baz_ref = DexMethod::get_method(
        "Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/String;)V");
    ASSERT_NE(baz_ref, nullptr);
    auto baz = baz_ref->as_def();
    ASSERT_NE(baz, nullptr);
    std::unordered_set<DexMethod*> def_inlinables{baz};

    MultiMethodInliner inliner(scope, stores, def_inlinables,
                               concurrent_resolver, conf,
                               MultiMethodInlinerMode::IntraDex);
    inliner.inline_methods();
    ASSERT_EQ(inliner.get_inlined().size(), 1u);

    auto bar_ref = DexMethod::get_method(
        "Lcom/facebook/redextest/SourceBlocksTest;.bar:()V");
    ASSERT_NE(bar_ref, nullptr);
    auto bar = bar_ref->as_def();
    ASSERT_NE(bar, nullptr);

    std::unordered_set<DexMethodRef*> seen_methods;
    {
      cfg::ScopedCFG cfg{bar->get_code()};
      for (const auto* b : cfg->blocks()) {
        for (const auto& mie : *b) {
          if (mie.type != MFLOW_SOURCE_BLOCK) {
            continue;
          }
          ASSERT_TRUE(mie.src_block != nullptr);
          seen_methods.insert(mie.src_block->src);
        }
      }
    }
    EXPECT_EQ(seen_methods.size(), 2);
    EXPECT_EQ(seen_methods.count(bar_ref), 1);
    EXPECT_EQ(seen_methods.count(baz_ref), 1);

    std::string bar_str = assembler::to_string(bar->get_code());
    EXPECT_EQ(bar_str,
              "((load-param-object v1) (.dbg DBG_SET_PROLOGUE_END) (.pos:dbg_0 "
              "\"Lcom/facebook/redextest/SourceBlocksTest;.bar:()V\" "
              "SourceBlocksTest.java 18) (.src_block "
              "\"Lcom/facebook/redextest/SourceBlocksTest;.bar:()V\" 0 ())"
              " (const-string World) (move-result-pseudo-object v0) "
              "(move-object v2 v1) (move-object v3 v0) (.pos:dbg_1 "
              "\"Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/"
              "String;)V\" SourceBlocksTest.java 22 dbg_0) (.src_block "
              "\"Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/"
              "String;)V\" 0 ()) (iput-object v3 v2 "
              "\"Lcom/facebook/redextest/SourceBlocksTest;.mHello:Ljava/lang/"
              "String;\") (.pos:dbg_2 "
              "\"Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/"
              "String;)V\" SourceBlocksTest.java 23 dbg_0) (.pos:dbg_3 "
              "\"Lcom/facebook/redextest/SourceBlocksTest;.bar:()V\" "
              "SourceBlocksTest.java 19) (return-void))");

    // Also check the assembler in a full-circle check.
    auto code = assembler::ircode_from_string(bar_str);
    auto code_str = assembler::to_string(code.get());
    EXPECT_EQ(bar_str, code_str);
  }
}

TEST_F(SourceBlocksTest, source_blocks_insert_after_exc) {
  auto type = DexType::get_type("Lcom/facebook/redextest/SourceBlocksTest;");
  ASSERT_NE(type, nullptr);
  auto cls = type_class(type);
  ASSERT_NE(cls, nullptr);

  // Check that no code has source blocks so far.
  {
    for (const auto* m : cls->get_all_methods()) {
      if (m->get_code() == nullptr) {
        continue;
      }
      for (const auto& mie : *m->get_code()) {
        ASSERT_NE(mie.type, MFLOW_SOURCE_BLOCK);
      }
    }
  }

  // Run the pass, check that each block has some SourceBlocks.
  {
    InsertSourceBlocksPass isbp{};
    run_passes({&isbp}, nullptr, Json::nullValue, [&](const auto&) {
      enable_pass(isbp);
      enable_always_inject(isbp);
      set_insert_after_excs(isbp, true);
    });
  }

  const std::unordered_map<std::string, size_t> kMaxSeen = {
      {"Lcom/facebook/redextest/SourceBlocksTest;.bar:()V", 3},
      {"Lcom/facebook/redextest/SourceBlocksTest;.foo:()V", 4},
      {"Lcom/facebook/redextest/SourceBlocksTest;.<init>:()V", 3},
      {"Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/String;)V",
       2},
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazz:()V", 2},
  };

  for (auto* m : cls->get_all_methods()) {
    if (m->get_code() == nullptr) {
      continue;
    }
    cfg::ScopedCFG cfg{m->get_code()};
    std::unordered_set<uint32_t> seen_ids;
    size_t max_seen{0};
    for (const auto* b : cfg->blocks()) {
      bool seen_source_block_in_b{false};
      size_t b_seen{0};
      for (const auto& mie : *b) {
        if (mie.type != MFLOW_SOURCE_BLOCK) {
          continue;
        }

        ++b_seen;

        ASSERT_TRUE(mie.src_block != nullptr);

        EXPECT_EQ(seen_ids.count(mie.src_block->id), 0u);
        seen_ids.insert(mie.src_block->id);

        EXPECT_EQ(mie.src_block->src, m);
      }
      EXPECT_GT(b_seen, 0u);
      max_seen = std::max(max_seen, b_seen);
    }
    auto it = kMaxSeen.find(show(m));
    if (it == kMaxSeen.end()) {
      ADD_FAILURE() << "Could not find expectation for " << show(m) << ": "
                    << max_seen;
      continue;
    }
    EXPECT_EQ(max_seen, it->second);
  }
}

TEST_F(SourceBlocksTest, source_blocks_profile) {
  auto profile_path = std::getenv("profile");
  ASSERT_NE(profile_path, nullptr) << "Missing profile path.";

  auto type = DexType::get_type("Lcom/facebook/redextest/SourceBlocksTest;");
  ASSERT_NE(type, nullptr);
  auto cls = type_class(type);
  ASSERT_NE(cls, nullptr);

  // Check that no code has source blocks so far.
  {
    for (const auto* m : cls->get_all_methods()) {
      if (m->get_code() == nullptr) {
        continue;
      }
      for (const auto& mie : *m->get_code()) {
        ASSERT_NE(mie.type, MFLOW_SOURCE_BLOCK);
      }
    }
  }

  // Run the pass, check that each block has a SourceBlock.
  InsertSourceBlocksPass isbp{};
  run_passes({&isbp}, nullptr, Json::nullValue, [&](const auto&) {
    enable_pass(isbp);
    set_insert_after_excs(isbp, false);
    set_profile(isbp, profile_path);
    set_force_serialize(isbp);
  });

  std::unordered_map<std::string, std::string> kExpectations = {
      {"Lcom/facebook/redextest/SourceBlocksTest;.bar:()V", "B0: 0(0.1:0.2)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.foo:()V", "B0: 0(0.2:0.3)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.<init>:()V",
       "B0: 0(0.3:0.4)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/String;)V",
       "B0: 0(0.4:0.5)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazz:()V", "B0: 0(0:0)"},
  };

  for (auto* m : cls->get_all_methods()) {
    if (m->get_code() == nullptr) {
      continue;
    }
    cfg::ScopedCFG cfg{m->get_code()};
    auto actual = get_blocks_as_txt(*cfg);
    auto it = kExpectations.find(show(m));
    if (it == kExpectations.end()) {
      EXPECT_TRUE(false) << "No expectation for " << show(m) << ": " << actual;
      continue;
    }
    EXPECT_EQ(actual, it->second) << show(m);
  }
}

TEST_F(SourceBlocksTest, source_blocks_profile_no_always_inject) {
  auto profile_path = std::getenv("profile");
  ASSERT_NE(profile_path, nullptr) << "Missing profile path.";

  auto type = DexType::get_type("Lcom/facebook/redextest/SourceBlocksTest;");
  ASSERT_NE(type, nullptr);
  auto cls = type_class(type);
  ASSERT_NE(cls, nullptr);

  // Check that no code has source blocks so far.
  {
    for (const auto* m : cls->get_all_methods()) {
      if (m->get_code() == nullptr) {
        continue;
      }
      for (const auto& mie : *m->get_code()) {
        ASSERT_NE(mie.type, MFLOW_SOURCE_BLOCK);
      }
    }
  }

  // Run the pass, check that each block has a SourceBlock.
  InsertSourceBlocksPass isbp{};
  run_passes({&isbp}, nullptr, Json::nullValue, [&](const auto&) {
    enable_pass(isbp);
    disable_always_inject(isbp);
    set_insert_after_excs(isbp, false);
    set_profile(isbp, profile_path);
  });

  std::unordered_map<std::string, std::string> kExpectations = {
      {"Lcom/facebook/redextest/SourceBlocksTest;.bar:()V", "B0: 0(0.1:0.2)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.foo:()V", "B0: 0(0.2:0.3)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.<init>:()V",
       "B0: 0(0.3:0.4)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/String;)V",
       "B0: 0(0.4:0.5)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazz:()V", "B0:"},
  };

  for (auto* m : cls->get_all_methods()) {
    if (m->get_code() == nullptr) {
      continue;
    }
    cfg::ScopedCFG cfg{m->get_code()};
    auto actual = get_blocks_as_txt(*cfg);
    auto it = kExpectations.find(show(m));
    if (it == kExpectations.end()) {
      EXPECT_TRUE(false) << "No expectation for " << show(m) << ": " << actual;
      continue;
    }
    EXPECT_EQ(actual, it->second) << show(m);
  }
}

TEST_F(SourceBlocksTest, source_blocks_profile_exc) {
  auto profile_path = std::getenv("profile2");
  ASSERT_NE(profile_path, nullptr) << "Missing profile2 path.";

  auto type = DexType::get_type("Lcom/facebook/redextest/SourceBlocksTest;");
  ASSERT_NE(type, nullptr);
  auto cls = type_class(type);
  ASSERT_NE(cls, nullptr);

  // Check that no code has source blocks so far.
  {
    for (const auto* m : cls->get_all_methods()) {
      if (m->get_code() == nullptr) {
        continue;
      }
      for (const auto& mie : *m->get_code()) {
        ASSERT_NE(mie.type, MFLOW_SOURCE_BLOCK);
      }
    }
  }

  // Run the pass, check that each block has a SourceBlock.
  InsertSourceBlocksPass isbp{};
  run_passes({&isbp}, nullptr, Json::nullValue, [&](const auto&) {
    enable_pass(isbp);
    set_insert_after_excs(isbp, true);
    set_profile(isbp, profile_path);
    set_force_serialize(isbp);
  });

  std::unordered_map<std::string, std::string> kExpectations = {
      {"Lcom/facebook/redextest/SourceBlocksTest;.bar:()V",
       "B0: 0(0.4:0.6) 1(0.5:0.5) 2(0.6:0.4)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.foo:()V",
       "B0: 0(0:0.3) 1(0.1:0.2) 2(0.2:0.1) 3(0.3:0)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.<init>:()V",
       "B0: 0(0.1:0.3) 1(0.2:0.2) 2(0.3:0.1)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/String;)V",
       "B0: 0(0.7:0.1) 1(0.8:0.2)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazz:()V",
       "B0: 0(0:0) 1(0:0)"},
  };

  for (auto* m : cls->get_all_methods()) {
    if (m->get_code() == nullptr) {
      continue;
    }
    cfg::ScopedCFG cfg{m->get_code()};
    auto actual = get_blocks_as_txt(*cfg);
    auto it = kExpectations.find(show(m));
    if (it == kExpectations.end()) {
      EXPECT_TRUE(false) << "No expectation for " << show(m) << ": " << actual;
      continue;
    }
    EXPECT_EQ(actual, it->second) << show(m);
  }
}

TEST_F(SourceBlocksTest, source_blocks_profile_exc_no_always_inject) {
  auto profile_path = std::getenv("profile2");
  ASSERT_NE(profile_path, nullptr) << "Missing profile2 path.";

  auto type = DexType::get_type("Lcom/facebook/redextest/SourceBlocksTest;");
  ASSERT_NE(type, nullptr);
  auto cls = type_class(type);
  ASSERT_NE(cls, nullptr);

  // Check that no code has source blocks so far.
  {
    for (const auto* m : cls->get_all_methods()) {
      if (m->get_code() == nullptr) {
        continue;
      }
      for (const auto& mie : *m->get_code()) {
        ASSERT_NE(mie.type, MFLOW_SOURCE_BLOCK);
      }
    }
  }

  // Run the pass, check that each block has a SourceBlock.
  InsertSourceBlocksPass isbp{};
  run_passes({&isbp}, nullptr, Json::nullValue, [&](const auto&) {
    enable_pass(isbp);
    disable_always_inject(isbp);
    set_insert_after_excs(isbp, true);
    set_profile(isbp, profile_path);
  });

  std::unordered_map<std::string, std::string> kExpectations = {
      {"Lcom/facebook/redextest/SourceBlocksTest;.bar:()V",
       "B0: 0(0.4:0.6) 1(0.5:0.5) 2(0.6:0.4)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.foo:()V",
       "B0: 0(0:0.3) 1(0.1:0.2) 2(0.2:0.1) 3(0.3:0)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.<init>:()V",
       "B0: 0(0.1:0.3) 1(0.2:0.2) 2(0.3:0.1)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/String;)V",
       "B0: 0(0.7:0.1) 1(0.8:0.2)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazz:()V", "B0:"},
  };

  for (auto* m : cls->get_all_methods()) {
    if (m->get_code() == nullptr) {
      continue;
    }
    cfg::ScopedCFG cfg{m->get_code()};
    auto actual = get_blocks_as_txt(*cfg);
    auto it = kExpectations.find(show(m));
    if (it == kExpectations.end()) {
      EXPECT_TRUE(false) << "No expectation for " << show(m) << ": " << actual;
      continue;
    }
    EXPECT_EQ(actual, it->second) << show(m);
  }
}

TEST_F(SourceBlocksTest, source_blocks_profile_always_inject_method_profiles) {
  auto profile_path = std::getenv("profile3");
  ASSERT_NE(profile_path, nullptr) << "Missing profile path.";

  auto method_profile_path = std::getenv("method-profile");
  ASSERT_NE(method_profile_path, nullptr) << "Missing method-profile path.";

  auto type = DexType::get_type("Lcom/facebook/redextest/SourceBlocksTest;");
  ASSERT_NE(type, nullptr);
  auto cls = type_class(type);
  ASSERT_NE(cls, nullptr);

  // Check that no code has source blocks so far.
  {
    for (const auto* m : cls->get_all_methods()) {
      if (m->get_code() == nullptr) {
        continue;
      }
      for (const auto& mie : *m->get_code()) {
        ASSERT_NE(mie.type, MFLOW_SOURCE_BLOCK);
      }
    }
  }

  // Need to set up a configuration that will load method profiles.
  Json::Value mp_val = Json::arrayValue;
  mp_val.resize(1);
  mp_val[0] = method_profile_path;
  Json::Value conf_val = Json::objectValue;
  conf_val["agg_method_stats_files"] = mp_val;

  // Run the pass, check that each block has a SourceBlock.
  InsertSourceBlocksPass isbp{};
  run_passes({&isbp}, nullptr, conf_val, [&](const auto&) {
    enable_pass(isbp);
    enable_always_inject(isbp);
    set_insert_after_excs(isbp, false);
    set_profile(isbp, profile_path);
  });

  std::unordered_map<std::string, std::string> kExpectations = {
      {"Lcom/facebook/redextest/SourceBlocksTest;.bar:()V", "B0: 0(0.1:0.2)"},
      // This comes from method profiles.
      {"Lcom/facebook/redextest/SourceBlocksTest;.foo:()V", "B0: 0(1:99)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.<init>:()V",
       "B0: 0(0.3:0.4)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/String;)V",
       "B0: 0(0.4:0.5)"},
      // This comes from method profiles.
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazz:()V", "B0: 0(1:98)"},
  };

  for (auto* m : cls->get_all_methods()) {
    if (m->get_code() == nullptr) {
      continue;
    }
    cfg::ScopedCFG cfg{m->get_code()};
    auto actual = get_blocks_as_txt(*cfg);
    auto it = kExpectations.find(show(m));
    if (it == kExpectations.end()) {
      EXPECT_TRUE(false) << "No expectation for " << show(m) << ": " << actual;
      continue;
    }
    EXPECT_EQ(actual, it->second) << show(m);
  }
}
