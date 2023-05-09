/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <memory>
#include <unordered_map>

#include <gtest/gtest.h>

#include "MethodSplitter.h"

#include "Creators.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "RedexTest.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

class MethodSplitterTest : public RedexTest {
 public:
  static std::pair<DexClass*, DexMethod*> create(const std::string& sig,
                                                 const std::string& code_str) {
    // Create a totally new class.
    size_t c = s_counter.fetch_add(1);
    std::string name = std::string("LFoo") + std::to_string(c) + ";";
    ClassCreator cc{DexType::make_type(name.c_str())};
    cc.set_super(type::java_lang_Object());

    auto m =
        DexMethod::make_method(name + ".bar:" + sig)
            ->make_concrete(ACC_PUBLIC | ACC_STATIC,
                            assembler::ircode_from_string(code_str), false);
    m->set_deobfuscated_name(show(m));
    cc.add_method(m);
    return std::make_pair(cc.create(), m);
  }

  static std::string replace_count(const std::string& str, DexMethod* m) {
    const std::string replacement = "LFoo;";
    const auto needle = m->get_class()->str();
    std::string res = str;
    for (;;) {
      size_t i = res.find(needle);
      if (i == std::string::npos) {
        break;
      }
      res.replace(i, needle.length(), replacement);
    }
    return res;
  }

  static std::pair<std::string, std::string> pair(std::string l,
                                                  std::string r) {
    return std::make_pair(std::move(l), std::move(r));
  }

  static method_splitting_impl::Config defaultConfig() {
    method_splitting_impl::Config config;
    // set some low limits...
    config.split_block_size = 4;
    config.min_original_size = 1;
    config.min_cold_split_size = 4;
    config.max_overhead_ratio = 0.5;
    config.max_iteration = 1;
    config.cost_split_method = 1;
    config.cost_split_switch = 1;
    return config;
  }

  static ::testing::AssertionResult test(
      const std::string& sig,
      const std::string& code_str,
      const method_splitting_impl::Config& config,
      std::initializer_list<std::pair<std::string, std::string>> expected) {
    auto [cls, m] = create(sig, code_str);
    m->get_code()->build_cfg();
    DexStoresVector stores;
    stores.emplace_back("test_store");
    stores.front().get_dexen().push_back({cls});
    method_splitting_impl::Stats stats;
    method_splitting_impl::split_methods_in_stores(
        stores, /* min_sdk */ 0, config,
        /* create_init_class_insns */ false,
        /* reserved_mrefs */ 0, /* reserved_trefs */ 0, &stats);
    m->get_code()->cfg().simplify();
    for (auto* out : stats.added_methods) {
      out->get_code()->cfg().simplify();
    }
    std::unordered_map<std::string, std::string> expected_map;
    for (const auto& p : expected) {
      expected_map.insert(p);
    }
    expected_map.emplace("", code_str);
    auto compare = [&](DexMethod* m) {
      const auto name = m->str();
      size_t index = name.find('$');
      std::string suffix;
      if (index != std::string::npos) {
        suffix = name.substr(index + 1);
      }
      auto it = expected_map.find(suffix);
      if (it == expected_map.end()) {
        return show(m) + "(" + suffix + ") not expected.";
      }
      if (m->get_code()->cfg_built()) {
        m->get_code()->clear_cfg();
      }
      std::string out_str =
          replace_count(assembler::to_string(m->get_code()), m);
      auto exp_ir = assembler::ircode_from_string(it->second);
      std::string exp_str = assembler::to_string(exp_ir.get());
      if (out_str != exp_str) {
        return "Actual:\n" + out_str + "\nExpected:\n" + exp_str;
      }
      return std::string();
    };
    auto main_error = compare(m);
    if (!main_error.empty()) {
      return ::testing::AssertionFailure() << show(m) << ": " << main_error;
    }
    std::vector<DexMethod*> ordered(stats.added_methods.begin(),
                                    stats.added_methods.end());
    std::sort(ordered.begin(), ordered.end(), compare_dexmethods);
    for (auto out : ordered) {
      auto out_error = compare(out);
      if (!out_error.empty()) {
        return ::testing::AssertionFailure() << show(out) << ": " << out_error;
      }
    }
    if (stats.added_methods.size() + 1 != expected_map.size()) {
      return ::testing::AssertionFailure() << "Unexpected amount of methods";
    }
    return ::testing::AssertionSuccess();
  }

 private:
  static std::atomic<size_t> s_counter;
};
std::atomic<size_t> MethodSplitterTest::s_counter{0};

TEST_F(MethodSplitterTest, NothingToDo) {
  auto before = R"(
    (
      (return-void)
    ))";
  auto after = before;
  auto res = test("()V",
                  before,
                  defaultConfig(),
                  {std::make_pair<std::string, std::string>("", after)});
  ASSERT_TRUE(res);
}

TEST_F(MethodSplitterTest, SplitLargeBlock) {
  auto before = R"(
    (
      (load-param v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    ))";
  auto after = R"(
    (
      (load-param v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (.pos:dbg_0 "LFoo;.bar:(I)I" RedexGenerated 0)
      (invoke-static (v0) "LFoo;.bar$split$cold0:(I)I")
      (move-result v0)
      (return v0)
    ))";
  auto split0 = R"(
    (
      (load-param v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    ))";
  auto res =
      test("(I)I",
           before,
           defaultConfig(),
           {std::make_pair<std::string, std::string>("", after),
            std::make_pair<std::string, std::string>("split$cold0", split0)});
  ASSERT_TRUE(res);
}

TEST_F(MethodSplitterTest, SplitConstants) {
  auto before = R"(
    (
      (load-param v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (const v1 1)
      (const v2 2)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v1)
      (add-int v0 v0 v2)
      (return v0)
    ))";
  auto after = R"(
    (
      (load-param v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (const v1 1)
      (const v2 2)
      (.pos:dbg_0 "LFoo;.bar:(I)I" RedexGenerated 0)
      (invoke-static (v0) "LFoo;.bar$split$cold0:(I)I")
      (move-result v0)
      (return v0)
    ))";
  auto split0 = R"(
    (
      (load-param v0)
      (const v1 1)
      (const v2 2)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v1)
      (add-int v0 v0 v2)
      (return v0)
    ))";
  auto res =
      test("(I)I",
           before,
           defaultConfig(),
           {std::make_pair<std::string, std::string>("", after),
            std::make_pair<std::string, std::string>("split$cold0", split0)});
  ASSERT_TRUE(res);
}

TEST_F(MethodSplitterTest, CannotSplitUninitializedObject) {
  auto before = R"(
    (
      (load-param v0)
      (new-instance "Ljava/lang/Object;")
      (move-result-pseudo-object v1)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (invoke-direct (v1) "Ljava/lang/Object;.<init>:()V")
      (return v0)
    ))";
  auto after = before;
  auto res = test("(I)I",
                  before,
                  defaultConfig(),
                  {std::make_pair<std::string, std::string>("", after)});
  ASSERT_TRUE(res);
}

TEST_F(MethodSplitterTest, SplitBranchFallthrough) {
  auto before = R"(
    (
      (load-param v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (if-eqz v0 :L0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
    (:L0)
      (return v0)
    ))";
  auto after = R"(
    (
      (load-param v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (if-eqz v0 :L0)
      (.pos:dbg_0 "LFoo;.bar:(I)I" RedexGenerated 0)
      (invoke-static (v0) "LFoo;.bar$split$cold0:(I)I")
      (move-result v0)
      (return v0)
    (:L0)
      (return v0)
    ))";
  auto split0 = R"(
    (
      (load-param v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    ))";
  auto res =
      test("(I)I",
           before,
           defaultConfig(),
           {std::make_pair<std::string, std::string>("", after),
            std::make_pair<std::string, std::string>("split$cold0", split0)});
  ASSERT_TRUE(res);
}

TEST_F(MethodSplitterTest, SplitSwitch) {
  auto before = R"(
    (
      (load-param v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (switch v0 (:a :b :c :d))
      (return v0)
    (:a 0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    (:b 1)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    (:c 2)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    (:d 3)
      (add-int v0 v0 v0)
      (return v0)
    ))";
  auto after = R"(
    (
      (load-param v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (switch v0 (:b :a))
      (.pos:dbg_0 "LFoo;.bar:(I)I" RedexGenerated 0)
      (invoke-static (v0) "LFoo;.bar$split$cold0:(I)I")
      (move-result v0)
      (return v0)
    (:b 1)
      (invoke-static (v0) "LFoo;.bar$split$cold2:(I)I")
      (move-result v0)
      (return v0)
    (:a 0)
      (invoke-static (v0) "LFoo;.bar$split$cold1:(I)I")
      (move-result v0)
      (return v0)
    ))";
  auto split0cd = R"(
    (
      (load-param v0)
      (switch v0 (:d :c))
      (return v0)
    (:d 3)
      (add-int v0 v0 v0)
      (return v0)
    (:c 2)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    ))";
  auto split1a = R"(
    (
      (load-param v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    ))";
  auto split2b = R"(
    (
      (load-param v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    ))";
  auto res =
      test("(I)I",
           before,
           defaultConfig(),
           {std::make_pair<std::string, std::string>("", after),
            std::make_pair<std::string, std::string>("split$cold0", split0cd),
            std::make_pair<std::string, std::string>("split$cold1", split1a),
            std::make_pair<std::string, std::string>("split$cold2", split2b)});
  ASSERT_TRUE(res);
}

TEST_F(MethodSplitterTest, SplitHotColdSwitch) {
  auto before = R"(
    (
      (load-param v0)
      (.src_block "LFoo;.bar:()V" 1 (0.5 0.5))
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (switch v0 (:a :b :c :d))
    (:fallthrough)
      (.src_block "LFoo;.bar:()V" 2 (0.0 0.0))
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    (:a 0)
      (.src_block "LFoo;.bar:()V" 3 (0.5 0.5))
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    (:b 1)
      (.src_block "LFoo;.bar:()V" 4 (0.0 0.0))
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    (:c 2)
      (.src_block "LFoo;.bar:()V" 5 (0.5 0.5))
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    (:d 3)
      (.src_block "LFoo;.bar:()V" 6 (0.0 0.0))
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    ))";
  auto after = R"(
    (
      (load-param v0)
      (.src_block "LFoo;.bar:()V" 1 (0.5 0.5))
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (switch v0 (:c :a))

      (.src_block "LFoo;.bar:(I)I" 4294967295 (0.0 0.0))
      (.pos:dbg_0 "LFoo;.bar:(I)I" RedexGenerated 0)
      (invoke-static (v0) "LFoo;.bar$split$hot_cold0:(I)I")
      (move-result v0)
      (return v0)
    (:c 2)
      (.src_block "LFoo;.bar:()V" 5 (0.5 0.5))
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    (:a 0)
      (.src_block "LFoo;.bar:()V" 3 (0.5 0.5))
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    ))";
  auto split0 = R"(
    (
      (load-param v0)
      (.src_block "LFoo;.bar$split$hot_cold0:(I)I" 4294967295 (0.0 0.0))
      (switch v0 (:L0 :L1))

      (.src_block "LFoo;.bar:()V" 2 (0.0 0.0))
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    (:L0 3)
      (.src_block "LFoo;.bar:()V" 6 (0.0 0.0))
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    (:L1 1)
      (.src_block "LFoo;.bar:()V" 4 (0.0 0.0))
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    ))";
  auto config = defaultConfig();
  config.min_hot_split_size = 16;
  config.min_hot_cold_split_size = 8;
  config.min_cold_split_size = 1000;
  config.max_overhead_ratio = 0.8;
  auto res = test(
      "(I)I",
      before,
      config,
      {std::make_pair<std::string, std::string>("", after),
       std::make_pair<std::string, std::string>("split$hot_cold0", split0)});
  ASSERT_TRUE(res);
}
