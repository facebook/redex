/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <gtest/gtest.h>

#include "ClassMerging.h"
#include "ConfigFiles.h"
#include "DexStore.h"
#include "HotColdGrouping.h"
#include "IRAssembler.h"
#include "IRTemplate.h"
#include "MethodProfiles.h"
#include "Model.h"
#include "PassManager.h"
#include "RedexTest.h"
#include "RedexTestUtils.h"
#include "ScopeHelper.h"
#include "TypeSystem.h"
#include "TypeUtil.h"
#include "VirtualScopes.h"

using namespace class_merging;

namespace {

constexpr std::string_view kCtor = R"(
  (method (public constructor) "$CLS.<init>:()V"
   (
    (load-param-object v0)
    (invoke-direct (v0) "$SUPER.<init>:()V")
    (return-void)
   )
  )
)";

constexpr std::string_view kVirtual = R"(
  (method (public) "$CLS.$NAME:()V"
   (
    (load-param-object v0)
    (return-void)
   )
  )
)";

DexMethod* add_method(DexClass* cls, const std::string& code) {
  auto* method = assembler::method_from_string(code);
  method->get_code()->build_cfg();
  cls->add_method(method);
  return method;
}

DexMethod* add_ctor(DexClass* cls) {
  const auto cls_name = cls->get_type()->str_copy();
  const auto super_name = cls->get_super_class()->str_copy();
  return add_method(cls,
                    ir(kCtor, {{"$CLS", cls_name}, {"$SUPER", super_name}}));
}

DexMethod* add_virtual(DexClass* cls, const std::string& name) {
  const auto cls_name = cls->get_type()->str_copy();
  return add_method(cls, ir(kVirtual, {{"$CLS", cls_name}, {"$NAME", name}}));
}

class ClassMergingHotColdGroupingTest : public RedexTest {
 protected:
  void SetUp() override {
    m_scope = create_empty_scope();
    m_base = make_class("LBase;", type::java_lang_Object());
    add_ctor(m_base);
    add_virtual(m_base, "run");
  }

  DexClass* make_class(const char* name, DexType* super) {
    auto* cls = create_internal_class(DexType::make_type(name), super, {});
    m_scope.push_back(cls);
    return cls;
  }

  // A subclass of Base with a ctor and an override of Base.run(), either of
  // which can be marked hot.
  DexClass* make_mergeable(const std::string& name,
                           bool hot_ctor = false,
                           bool hot_run = false) {
    auto* cls = make_class(name.c_str(), m_base->get_type());
    auto* ctor = add_ctor(cls);
    auto* run = add_virtual(cls, "run");
    if (hot_ctor) {
      m_hot_methods.push_back(ctor);
    }
    if (hot_run) {
      m_hot_methods.push_back(run);
    }
    return cls;
  }

  bool is_hot(const DexClass* cls) {
    virtual_scope::VirtualScopes vscopes(m_scope);
    UnorderedSet<const DexMethod*> hot_methods(m_hot_methods.begin(),
                                               m_hot_methods.end());
    return is_hot_mergeable(cls, vscopes, hot_methods);
  }

  // Builds the model for all subclasses of Base, with `m_hot_methods` hot in
  // the method profiles, and returns the sorted mergeables of each merger.
  std::vector<std::vector<std::string>> merger_groups(
      bool hot_cold_grouping, ModelStats* stats = nullptr) {
    DexStore store("classes");
    store.add_classes(m_scope);
    DexStoresVector stores{store};
    // java.lang.Object is external, as in real builds, so RefChecker only
    // accepts hierarchies rooted at it when the SDK API list has it.
    auto tmp_dir = redex::make_tmp_dir("hot_cold_grouping_test_%%%%%%%%");
    const auto api_file = tmp_dir.path + "/api.txt";
    {
      std::ofstream out(api_file);
      out << "Ljava/lang/Object; 1 Ljava/lang/Object; 1 0\n"
          << "  M Ljava/lang/Object;.<init>:()V 1\n";
    }
    Json::Value json;
    json["android_sdk_api_21_file"] = api_file;
    ConfigFiles conf(json);
    method_profiles::Stats hot_stats;
    hot_stats.appear_percent = 100.0;
    hot_stats.call_count = 100.0;
    UnorderedMap<const DexMethodRef*, method_profiles::Stats> manual_hot_stats;
    for (auto* m : m_hot_methods) {
      manual_hot_stats[m] = hot_stats;
    }
    conf.get_method_profiles() = method_profiles::MethodProfiles::initialize(
        "manual_hot", manual_hot_stats);
    RedexOptions options;
    options.min_sdk = 21;
    PassManager mgr({}, conf, options);

    ModelSpec spec;
    spec.name = "HotCold";
    spec.class_name_prefix = "HotCold";
    spec.roots.insert(m_base->get_type());
    spec.include_primary_dex = true;
    spec.hot_cold_grouping = hot_cold_grouping;

    TypeSystem type_system(m_scope);
    load_roots_subtypes_as_merging_targets(type_system, &spec);
    virtual_scope::VirtualScopes vscopes(m_scope);
    auto model =
        construct_model(type_system, vscopes, m_scope, conf, mgr, stores, spec);
    if (stats != nullptr) {
      *stats = model.get_model_stats();
    }

    std::vector<std::vector<std::string>> groups;
    model.walk_hierarchy([&](const MergerType& merger) {
      if (merger.mergeables.empty()) {
        return;
      }
      std::vector<std::string> group;
      for (const auto* type : merger.mergeables) {
        group.push_back(type->str_copy());
      }
      std::sort(group.begin(), group.end());
      groups.push_back(std::move(group));
    });
    std::sort(groups.begin(), groups.end());
    return groups;
  }

  Scope m_scope;
  DexClass* m_base{nullptr};
  std::vector<DexMethod*> m_hot_methods;
};

} // namespace

TEST_F(ClassMergingHotColdGroupingTest, NoHotMethodIsCold) {
  auto* a = make_mergeable("LA;");
  EXPECT_FALSE(is_hot(a));
}

TEST_F(ClassMergingHotColdGroupingTest, HotCtorIsHot) {
  auto* a = make_mergeable("LA;", /* hot_ctor */ true);
  EXPECT_TRUE(is_hot(a));
}

TEST_F(ClassMergingHotColdGroupingTest, HotOverrideIsHot) {
  auto* a = make_mergeable("LA;", /* hot_ctor */ false, /* hot_run */ true);
  EXPECT_TRUE(is_hot(a));
}

// A virtual method only A declares is effectively final: merging relocates it
// instead of folding it into a dispatch, so its hotness does not count.
TEST_F(ClassMergingHotColdGroupingTest, HotEffectivelyFinalMethodIsIgnored) {
  auto* a = make_mergeable("LA;");
  m_hot_methods.push_back(add_virtual(a, "helper"));
  EXPECT_FALSE(is_hot(a));
}

TEST_F(ClassMergingHotColdGroupingTest, DisabledKeepsOneGroup) {
  make_mergeable("LA;", false, true);
  make_mergeable("LB;", false, true);
  make_mergeable("LC;");
  make_mergeable("LD;");

  ModelStats stats;
  EXPECT_EQ(
      merger_groups(false, &stats),
      (std::vector<std::vector<std::string>>{{"LA;", "LB;", "LC;", "LD;"}}));
  EXPECT_EQ(stats.m_hot_mergeables + stats.m_cold_mergeables, 0u);
}

TEST_F(ClassMergingHotColdGroupingTest, EnabledSplitsHotAndCold) {
  make_mergeable("LA;", false, true);
  make_mergeable("LB;", true);
  make_mergeable("LC;");
  make_mergeable("LD;");

  ModelStats stats;
  EXPECT_EQ(
      merger_groups(true, &stats),
      (std::vector<std::vector<std::string>>{{"LA;", "LB;"}, {"LC;", "LD;"}}));
  EXPECT_EQ(stats.m_hot_mergeables, 2u);
  EXPECT_EQ(stats.m_cold_mergeables, 2u);
  EXPECT_EQ(stats.m_hot_cold_split_groups, 1u);
  EXPECT_EQ(stats.m_hot_cold_dropped, 0u);
}

TEST_F(ClassMergingHotColdGroupingTest, UndersizedPartStaysUnmerged) {
  make_mergeable("LA;", false, true);
  make_mergeable("LB;");
  make_mergeable("LC;");
  make_mergeable("LD;");

  ModelStats stats;
  EXPECT_EQ(merger_groups(true, &stats),
            (std::vector<std::vector<std::string>>{{"LB;", "LC;", "LD;"}}));
  EXPECT_EQ(stats.m_hot_cold_dropped, 1u);
  EXPECT_EQ(stats.m_dropped, 1u);
}

TEST_F(ClassMergingHotColdGroupingTest, AllHotIsNotSplit) {
  make_mergeable("LA;", false, true);
  make_mergeable("LB;", false, true);
  make_mergeable("LC;", true);

  ModelStats stats;
  EXPECT_EQ(merger_groups(true, &stats),
            (std::vector<std::vector<std::string>>{{"LA;", "LB;", "LC;"}}));
  EXPECT_EQ(stats.m_hot_cold_split_groups, 0u);
}
