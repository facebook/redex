/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "RedexTest.h"

#include "AnalysisUsage.h"
#include "Pass.h"

struct AnalysisUsageTest : public RedexTest {
  template <typename P>
  static void run_invalidation_policy_by_pass(
      std::unordered_map<AnalysisID, Pass*>& preserved) {
    P* p = new P();
    AnalysisUsage au;
    p->set_analysis_usage(au);
    au.do_pass_invalidation(&preserved);
  }

  template <typename P>
  std::shared_ptr<int> get_analysis_result_for_pass(
      const std::unordered_map<AnalysisID, Pass*>& preserved) {
    P* p = static_cast<P*>(preserved.at(get_analysis_id_by_pass<P>()));
    always_assert(p);
    return p->get_result();
  }
};

class MyAnalysisPass : public Pass {
 public:
  using Result = int;

  MyAnalysisPass() : Pass("MyAnalysisPass", Pass::ANALYSIS) {}

  void run_pass(DexStoresVector& /* stores */,
                ConfigFiles& /* conf */,
                PassManager& /* mgr */) override {
    m_result = std::make_shared<Result>(42);
  }

  void set_result() { m_result = std::make_shared<Result>(42); }

  std::shared_ptr<Result> get_result() { return m_result; }

  void destroy_analysis_result() override { m_result = nullptr; }

 private:
  std::shared_ptr<Result> m_result = nullptr;
};

class MyAnalysisPass2 : public Pass {
 public:
  using Result = int;

  MyAnalysisPass2() : Pass("MyAnalysisPass2", Pass::ANALYSIS) {}

  void run_pass(DexStoresVector& /* stores */,
                ConfigFiles& /* conf */,
                PassManager& /* mgr */) override {
    m_result = std::make_shared<Result>(42);
  }

  void set_result() { m_result = std::make_shared<Result>(42); }

  std::shared_ptr<Result> get_result() { return m_result; }

  void destroy_analysis_result() override { m_result = nullptr; }

 private:
  std::shared_ptr<Result> m_result = nullptr;
};

class ConsumeAnalysisAndInvalidatePass : public Pass {
 public:
  ConsumeAnalysisAndInvalidatePass()
      : Pass("ConsumeAnalysisAndInvalidatePass") {}

  void set_analysis_usage(AnalysisUsage& au) const override {
    au.add_required<MyAnalysisPass>();
  }

  void run_pass(DexStoresVector& /* stores */,
                ConfigFiles& /* conf */,
                PassManager& mgr) override {
    auto preserved = mgr.get_preserved_analysis<MyAnalysisPass>();
    always_assert(preserved);
    auto result = preserved->get_result();
    always_assert(result);
  }
};

class ConsumeAnalysisAndPreservePass : public Pass {
 public:
  ConsumeAnalysisAndPreservePass() : Pass("ConsumeAnalysisAndPreservePass") {}

  void set_analysis_usage(AnalysisUsage& au) const override {
    au.add_required<MyAnalysisPass>();
    au.set_preserve_all();
  }

  void run_pass(DexStoresVector& /* stores */,
                ConfigFiles& /* conf */,
                PassManager& mgr) override {
    auto preserved = mgr.get_preserved_analysis<MyAnalysisPass>();
    always_assert(preserved);
    auto result = preserved->get_result();
    always_assert(result);
  }
};

class ConsumeAnalysisAndPreserveOnePass : public Pass {
 public:
  ConsumeAnalysisAndPreserveOnePass()
      : Pass("ConsumeAnalysisAndPreserveOnePass") {}

  void set_analysis_usage(AnalysisUsage& au) const override {
    au.add_required<MyAnalysisPass>();
    au.add_preserve_specific<MyAnalysisPass>();
  }

  void run_pass(DexStoresVector& /* stores */,
                ConfigFiles& /* conf */,
                PassManager& mgr) override {
    auto preserved = mgr.get_preserved_analysis<MyAnalysisPass>();
    always_assert(preserved);
    auto result = preserved->get_result();
    always_assert(result);
  }
};

class ConsumeAnalysis2Pass : public Pass {
 public:
  ConsumeAnalysis2Pass() : Pass("ConsumeAnalysis2Pass") {}

  void set_analysis_usage(AnalysisUsage& au) const override {
    au.add_required<MyAnalysisPass2>();
  }

  void run_pass(DexStoresVector& /* stores */,
                ConfigFiles& /* conf */,
                PassManager& /* mgr */) override {}
};

TEST_F(AnalysisUsageTest, testAnalysisInvalidation) {
  auto get_preserved_passes = []() {
    std::unordered_map<AnalysisID, Pass*> ret;

    auto my = new MyAnalysisPass();
    my->set_result();
    ret.emplace(get_analysis_id_by_pass<MyAnalysisPass>(), my);
    auto my2 = new MyAnalysisPass2();
    my2->set_result();
    ret.emplace(get_analysis_id_by_pass<MyAnalysisPass2>(), my2);
    return ret;
  };

  {
    auto preserved = get_preserved_passes();
    EXPECT_EQ(preserved.size(), 2);

    run_invalidation_policy_by_pass<ConsumeAnalysisAndInvalidatePass>(
        preserved);
    EXPECT_EQ(preserved.size(), 0);
  }

  {
    auto preserved = get_preserved_passes();
    run_invalidation_policy_by_pass<ConsumeAnalysisAndPreservePass>(preserved);
    EXPECT_EQ(preserved.size(), 2);

    EXPECT_NE(nullptr, get_analysis_result_for_pass<MyAnalysisPass>(preserved));
    EXPECT_NE(nullptr,
              get_analysis_result_for_pass<MyAnalysisPass2>(preserved));
  }

  {
    auto preserved = get_preserved_passes();
    run_invalidation_policy_by_pass<ConsumeAnalysisAndPreserveOnePass>(
        preserved);
    EXPECT_EQ(preserved.size(), 1);

    EXPECT_TRUE(get_analysis_result_for_pass<MyAnalysisPass>(preserved));
    EXPECT_FALSE(preserved.count(get_analysis_id_by_pass<MyAnalysisPass2>()));
  }
}

TEST_F(AnalysisUsageTest, testDependencyChecking) {
  {
    std::vector<Pass*> valid_sequence{
        new MyAnalysisPass(),
        new MyAnalysisPass2(),
        new ConsumeAnalysisAndPreservePass(),
        new ConsumeAnalysisAndPreserveOnePass(),
        new ConsumeAnalysisAndInvalidatePass(),
    };
    AnalysisUsage::check_dependencies(valid_sequence);
  }

  {
    std::vector<Pass*> invalid_sequence{
        new MyAnalysisPass(),
        new MyAnalysisPass2(),
        new ConsumeAnalysisAndPreservePass(),
        new ConsumeAnalysisAndPreserveOnePass(),
        new ConsumeAnalysis2Pass(),
    };

    bool exception_caught = false;
    try {
      AnalysisUsage::check_dependencies(invalid_sequence);
    } catch (const RedexException& e) {
      exception_caught = true;
      EXPECT_EQ(e.type, UNSATISFIED_ANALYSIS_PASS);
    }
    EXPECT_TRUE(exception_caught);
  }
}
