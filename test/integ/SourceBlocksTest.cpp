/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InsertSourceBlocks.h"

#include <algorithm>

#include <gtest/gtest.h>

#include "DeterministicContainers.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "Inliner.h"
#include "InlinerConfig.h"
#include "RedexContext.h"
#include "RedexTest.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Walkers.h"

class SourceBlocksTest : public RedexIntegrationTest {
 public:
  SourceBlocksTest() {
    // The loading code in integ-test does not insert deobfuscated names.
    walk::methods(*classes, [](auto* m) {
      always_assert(m->get_deobfuscated_name_or_null() == nullptr);
      m->set_deobfuscated_name(show(m));
    });
  }

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

  template <bool kFull>
  std::string get_blocks_as_txt_impl(const cfg::ControlFlowGraph& cfg) {
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
        oss << " ";
        if (kFull) {
          oss << sb->src->str() << "@";
        }
        oss << sb->id;
        if (sb->vals_size > 0) {
          oss << "(";
          bool first_val = true;
          for (size_t i = 0; i < sb->vals_size; i++) {
            auto& val = sb->vals[i];
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

  std::string get_blocks_as_txt(const cfg::ControlFlowGraph& cfg) {
    return get_blocks_as_txt_impl<false>(cfg);
  }

  std::string get_blocks_as_txt_full(const cfg::ControlFlowGraph& cfg) {
    return get_blocks_as_txt_impl<true>(cfg);
  }

  void insert_source_block_vals(DexMethod* method,
                                const SourceBlock::Val& val) {
    cfg::ScopedCFG cfg{method->get_code()};
    uint32_t id = 0;
    for (auto* b : cfg->blocks()) {
      std::vector<SourceBlock::Val> vals{val};
      source_blocks::impl::BlockAccessor::push_source_block(
          b,
          std::make_unique<SourceBlock>(method->get_deobfuscated_name_or_null(),
                                        id++, std::move(vals)));
    }
  }

  bool has_any_source_block_positive_val(DexMethod* method) {
    for (auto& mie : *method->get_code()) {
      if (mie.type == MFLOW_SOURCE_BLOCK &&
          source_blocks::has_source_block_positive_val(mie.src_block.get())) {
        return true;
      }
    }
    return false;
  }

  bool have_all_source_block_positive_val(DexMethod* method) {
    for (auto& mie : *method->get_code()) {
      if (mie.type == MFLOW_SOURCE_BLOCK &&
          !source_blocks::has_source_block_positive_val(mie.src_block.get())) {
        return false;
      }
    }
    return true;
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
      UnorderedSet<uint32_t> seen_ids;
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

          if (m->get_name()->str().substr(0, 7) != "access$") {
            EXPECT_EQ(mie.src_block->src, m->get_deobfuscated_name_or_null());
          }
        }
        EXPECT_TRUE(seen_source_block_in_b);
      }
    }
  }

  // Run inliner, check that we have mix now.
  {
    inliner::InlinerConfig conf{};
    auto scope = build_class_scope(stores);
    walk::parallel::code(scope, [&](auto*, IRCode& code) { code.build_cfg(); });
    conf.populate(scope);
    init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
        scope, /* create_init_class_insns */ false);

    ConcurrentMethodResolver concurrent_method_resolver;

    auto baz_ref = DexMethod::get_method(
        "Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/"
        "String;)V");
    ASSERT_NE(baz_ref, nullptr);
    auto baz = baz_ref->as_def();
    ASSERT_NE(baz, nullptr);
    UnorderedSet<DexMethod*> def_inlinables{baz};

    int min_sdk = 0;
    MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                               def_inlinables,
                               std::ref(concurrent_method_resolver), conf,
                               min_sdk, MultiMethodInlinerMode::IntraDex);
    inliner.inline_methods();
    walk::parallel::code(scope, [&](auto*, IRCode& code) { code.clear_cfg(); });

    ASSERT_EQ(inliner.get_inlined().size(), 1u);

    auto bar_ref = DexMethod::get_method(
        "Lcom/facebook/redextest/SourceBlocksTest;.bar:()V");
    ASSERT_NE(bar_ref, nullptr);
    auto bar = bar_ref->as_def();
    ASSERT_NE(bar, nullptr);

    UnorderedSet<DexMethodRef*> seen_methods;
    {
      cfg::ScopedCFG cfg{bar->get_code()};
      for (const auto* b : cfg->blocks()) {
        for (const auto& mie : *b) {
          if (mie.type != MFLOW_SOURCE_BLOCK) {
            continue;
          }
          ASSERT_TRUE(mie.src_block != nullptr);
          seen_methods.insert(
              DexMethod::get_method(mie.src_block->src->str_copy()));
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

  const UnorderedMap<std::string, size_t> kMaxSeen = {
      {"Lcom/facebook/redextest/SourceBlocksTest;.bar:()V", 3},
      {"Lcom/facebook/redextest/SourceBlocksTest;.foo:()V", 4},
      {"Lcom/facebook/redextest/SourceBlocksTest;.<init>:()V", 3},
      {"Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/String;)V",
       2},
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazz:()V", 2},
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazzz:()V", 3},
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$002:(Ljava/lang/"
       "String;)Ljava/lang/String;",
       2},
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$100:()V", 2},
  };

  for (auto* m : cls->get_all_methods()) {
    if (m->get_code() == nullptr) {
      continue;
    }
    cfg::ScopedCFG cfg{m->get_code()};
    UnorderedSet<uint32_t> seen_ids;
    size_t max_seen{0};
    for (const auto* b : cfg->blocks()) {
      size_t b_seen{0};
      for (const auto& mie : *b) {
        if (mie.type != MFLOW_SOURCE_BLOCK) {
          continue;
        }

        ++b_seen;

        ASSERT_TRUE(mie.src_block != nullptr);

        EXPECT_EQ(seen_ids.count(mie.src_block->id), 0u);
        seen_ids.insert(mie.src_block->id);

        if (m->get_name()->str().substr(0, 7) != "access$") {
          EXPECT_EQ(mie.src_block->src, m->get_deobfuscated_name_or_null());
        }
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
  }
}

TEST_F(SourceBlocksTest, scaling) {
  g_redex->set_sb_interaction_index({{"Fake", 0}});

  auto type =
      DexType::get_type("Lcom/facebook/redextest/SourceBlocksTest$Scaling;");
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

  // The incoming profile doesn't actually matter, we tweak here what matters:

  auto no_source_blocks_method_ref = DexMethod::get_method(
      "Lcom/facebook/redextest/SourceBlocksTest$Scaling;.no_source_blocks:()V");
  ASSERT_NE(no_source_blocks_method_ref, nullptr);
  auto no_source_blocks_method = no_source_blocks_method_ref->as_def();
  ASSERT_NE(no_source_blocks_method, nullptr);
  ASSERT_FALSE(has_any_source_block_positive_val(no_source_blocks_method));

  auto nan_source_blocks_method_ref = DexMethod::get_method(
      "Lcom/facebook/redextest/"
      "SourceBlocksTest$Scaling;.nan_source_blocks:()V");
  ASSERT_NE(nan_source_blocks_method_ref, nullptr);
  auto nan_source_blocks_method = nan_source_blocks_method_ref->as_def();
  ASSERT_NE(nan_source_blocks_method, nullptr);
  insert_source_block_vals(
      nan_source_blocks_method,
      SourceBlock::Val(/* value */ NAN, /* appear100 */ 0.0f));
  ASSERT_FALSE(has_any_source_block_positive_val(nan_source_blocks_method));

  auto zero_source_blocks_method_ref = DexMethod::get_method(
      "Lcom/facebook/redextest/"
      "SourceBlocksTest$Scaling;.zero_source_blocks:()V");
  ASSERT_NE(zero_source_blocks_method_ref, nullptr);
  auto zero_source_blocks_method = zero_source_blocks_method_ref->as_def();
  ASSERT_NE(zero_source_blocks_method, nullptr);
  insert_source_block_vals(
      zero_source_blocks_method,
      SourceBlock::Val(/* value */ 0.0f, /* appear100 */ 0.0f));
  ASSERT_FALSE(has_any_source_block_positive_val(zero_source_blocks_method));

  auto hot_source_blocks_method_ref = DexMethod::get_method(
      "Lcom/facebook/redextest/"
      "SourceBlocksTest$Scaling;.hot_source_blocks:()V");
  ASSERT_NE(hot_source_blocks_method_ref, nullptr);
  auto hot_source_blocks_method = hot_source_blocks_method_ref->as_def();
  ASSERT_NE(hot_source_blocks_method, nullptr);
  insert_source_block_vals(
      hot_source_blocks_method,
      SourceBlock::Val(/* value */ 1.0f, /* appear100 */ 0.0f));
  ASSERT_TRUE(has_any_source_block_positive_val(hot_source_blocks_method));
  ASSERT_TRUE(have_all_source_block_positive_val(hot_source_blocks_method));

  auto hot_source_blocks_inlined_method_ref = DexMethod::get_method(
      "Lcom/facebook/redextest/"
      "SourceBlocksTest$Scaling;.hot_source_blocks_inlined:(Z)V");
  ASSERT_NE(hot_source_blocks_inlined_method_ref, nullptr);
  auto hot_source_blocks_inlined_method =
      hot_source_blocks_inlined_method_ref->as_def();
  ASSERT_NE(hot_source_blocks_inlined_method, nullptr);
  insert_source_block_vals(
      hot_source_blocks_inlined_method,
      SourceBlock::Val(/* value */ 1.0f, /* appear100 */ 0.0f));
  ASSERT_TRUE(
      has_any_source_block_positive_val(hot_source_blocks_inlined_method));
  ASSERT_TRUE(
      have_all_source_block_positive_val(hot_source_blocks_inlined_method));
  hot_source_blocks_inlined_method->rstate.set_force_inline();

  // Run inliner, check that we have mix now.
  {
    inliner::InlinerConfig conf{};
    auto scope = build_class_scope(stores);
    walk::parallel::code(scope, [&](auto*, IRCode& code) { code.build_cfg(); });
    conf.populate(scope);
    init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
        scope, /* create_init_class_insns */ false);

    ConcurrentMethodResolver concurrent_method_resolver;

    UnorderedSet<DexMethod*> def_inlinables{hot_source_blocks_inlined_method};

    int min_sdk = 0;
    MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                               def_inlinables,
                               std::ref(concurrent_method_resolver), conf,
                               min_sdk, MultiMethodInlinerMode::IntraDex);
    inliner.inline_methods();
    walk::parallel::code(scope, [&](auto*, IRCode& code) { code.clear_cfg(); });

    ASSERT_EQ(inliner.get_inlined().size(), 1u);
    ASSERT_EQ(inliner.get_info().calls_inlined, 4u);

    ASSERT_FALSE(has_any_source_block_positive_val(no_source_blocks_method));
    ASSERT_FALSE(has_any_source_block_positive_val(nan_source_blocks_method));
    ASSERT_FALSE(has_any_source_block_positive_val(zero_source_blocks_method));
    ASSERT_TRUE(has_any_source_block_positive_val(hot_source_blocks_method));
    ASSERT_TRUE(have_all_source_block_positive_val(hot_source_blocks_method));
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

  UnorderedMap<std::string, std::string> kExpectations = {
      {"Lcom/facebook/redextest/SourceBlocksTest;.bar:()V", "B0: 0(0.1:0.2)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.foo:()V", "B0: 0(0.2:0.3)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.<init>:()V",
       "B0: 0(0.3:0.4)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/String;)V",
       "B0: 0(0.4:0.5)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazz:()V", "B0: 0(0:0)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazzz:()V", "B0: 0(0:0)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$002:(Ljava/lang/"
       "String;)Ljava/lang/String;",
       "B0: 0(0:0)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$100:()V",
       "B0: 0(0:0)"},
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

  UnorderedMap<std::string, std::string> kExpectations = {
      {"Lcom/facebook/redextest/SourceBlocksTest;.bar:()V", "B0: 0(0.1:0.2)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.foo:()V", "B0: 0(0.2:0.3)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.<init>:()V",
       "B0: 0(0.3:0.4)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/String;)V",
       "B0: 0(0.4:0.5)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazz:()V", "B0:"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazzz:()V", "B0:"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$002:(Ljava/lang/"
       "String;)Ljava/lang/String;",
       "B0:"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$100:()V", "B0:"},
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

  UnorderedMap<std::string, std::string> kExpectations = {
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
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazzz:()V",
       "B0: 0(0:0) 1(0:0) 2(0:0)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$002:(Ljava/lang/"
       "String;)Ljava/lang/String;",
       "B0: 0(0:0) 1(0:0)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$100:()V",
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

  UnorderedMap<std::string, std::string> kExpectations = {
      {"Lcom/facebook/redextest/SourceBlocksTest;.bar:()V",
       "B0: 0(0.4:0.6) 1(0.5:0.5) 2(0.6:0.4)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.foo:()V",
       "B0: 0(0:0.3) 1(0.1:0.2) 2(0.2:0.1) 3(0.3:0)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.<init>:()V",
       "B0: 0(0.1:0.3) 1(0.2:0.2) 2(0.3:0.1)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/String;)V",
       "B0: 0(0.7:0.1) 1(0.8:0.2)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazz:()V", "B0:"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazzz:()V", "B0:"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$002:(Ljava/lang/"
       "String;)Ljava/lang/String;",
       "B0:"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$100:()V", "B0:"},
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

  UnorderedMap<std::string, std::string> kExpectations = {
      {"Lcom/facebook/redextest/SourceBlocksTest;.bar:()V", "B0: 0(0.1:0.2)"},
      // This comes from method profiles.
      {"Lcom/facebook/redextest/SourceBlocksTest;.foo:()V", "B0: 0(1:99)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.<init>:()V",
       "B0: 0(0.3:0.4)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.baz:(Ljava/lang/String;)V",
       "B0: 0(0.4:0.5)"},
      // This comes from method profiles.
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazz:()V", "B0: 0(1:98)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.bazzz:()V", "B0: 0(0:0)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$002:(Ljava/lang/"
       "String;)Ljava/lang/String;",
       "B0: 0(0:0)"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$100:()V",
       "B0: 0(0:0)"},
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

TEST_F(SourceBlocksTest, source_blocks_access_methods) {
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
    enable_always_inject(isbp);
    set_insert_after_excs(isbp, false);
  });

  UnorderedMap<std::string, std::string> kExpectations = {
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$002:(Ljava/lang/"
       "String;)Ljava/lang/String;",
       "B0: "
       "Lcom/facebook/redextest/"
       "SourceBlocksTest;.access$redex5be80c743d1d526b$02:(Ljava/lang/"
       "String;)Ljava/lang/String;@0"},
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$100:()V",
       "B0: "
       "Lcom/facebook/redextest/"
       "SourceBlocksTest;.access$redex1bc24000ccc37110$00:()V@0"},
  };

  for (auto* m : cls->get_all_methods()) {
    if (m->get_code() == nullptr) {
      continue;
    }
    if (m->get_name()->str().substr(0, 7) != "access$") {
      continue;
    }
    cfg::ScopedCFG cfg{m->get_code()};
    auto actual = get_blocks_as_txt_full(*cfg);
    auto it = kExpectations.find(show(m));
    if (it == kExpectations.end()) {
      EXPECT_TRUE(false) << "No expectation for " << show(m) << ": " << actual;
      continue;
    }
    EXPECT_EQ(actual, it->second) << show(m);
  }
}

namespace {
namespace access_methods {

struct Data {
  const char* profile;
  const char* field_access;
  const char* method_access;
};

} // namespace access_methods
} // namespace

class SourceBlocksAccessMethodsTest
    : public SourceBlocksTest,
      public testing::WithParamInterface<access_methods::Data> {};

TEST_P(SourceBlocksAccessMethodsTest, profile) {
  auto& param = GetParam();
  auto profile_path = std::getenv(param.profile);
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
    enable_always_inject(isbp);
    set_insert_after_excs(isbp, false);
    set_profile(isbp, profile_path);
  });

  UnorderedMap<std::string, std::string> kExpectations = {
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$002:(Ljava/lang/"
       "String;)Ljava/lang/String;",
       param.field_access},
      {"Lcom/facebook/redextest/SourceBlocksTest;.access$100:()V",
       param.method_access},
  };

  for (auto* m : cls->get_all_methods()) {
    if (m->get_code() == nullptr) {
      continue;
    }
    if (m->get_name()->str().substr(0, 7) != "access$") {
      continue;
    }
    cfg::ScopedCFG cfg{m->get_code()};
    auto actual = get_blocks_as_txt_full(*cfg);
    auto it = kExpectations.find(show(m));
    if (it == kExpectations.end()) {
      EXPECT_TRUE(false) << "No expectation for " << show(m) << ": " << actual;
      continue;
    }
    EXPECT_EQ(actual, it->second) << show(m);
  }
}

INSTANTIATE_TEST_SUITE_P(
    AccessMethodsGroup,
    SourceBlocksAccessMethodsTest,
    testing::ValuesIn(std::vector<access_methods::Data>{
        {"profile_access_exact",
         "B0: "
         "Lcom/facebook/redextest/"
         "SourceBlocksTest;.access$redex5be80c743d1d526b$02:(Ljava/lang/"
         "String;)Ljava/lang/String;@0(0.1:0.2)",
         "B0: "
         "Lcom/facebook/redextest/"
         "SourceBlocksTest;.access$redex1bc24000ccc37110$00:()V@0(0.3:0.4)"},
        {"profile_access_hash",
         "B0: "
         "Lcom/facebook/redextest/"
         "SourceBlocksTest;.access$redex5be80c743d1d526b$02:(Ljava/lang/"
         "String;)Ljava/lang/String;@0(0.6:0.7)",
         "B0: "
         "Lcom/facebook/redextest/"
         "SourceBlocksTest;.access$redex1bc24000ccc37110$00:()V@0(0.8:0.9)"},
        {"profile_access_both",
         // When both are available prefer hash.
         "B0: "
         "Lcom/facebook/redextest/"
         "SourceBlocksTest;.access$redex5be80c743d1d526b$02:(Ljava/lang/"
         "String;)Ljava/lang/String;@0(0.6:0.7)",
         "B0: "
         "Lcom/facebook/redextest/"
         "SourceBlocksTest;.access$redex1bc24000ccc37110$00:()V@0(0.8:0.9)"}}),
    [](const auto& info) { return info.param.profile; });
