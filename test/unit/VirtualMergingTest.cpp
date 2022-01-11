/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <string>
#include <unordered_map>

#include "ApiLevelChecker.h"
#include "Creators.h"
#include "DexClass.h"
#include "DexStore.h"
#include "IRAssembler.h"
#include "MethodProfiles.h"
#include "RedexTest.h"
#include "RegisterAllocation.h"
#include "StripDebugInfo.h"
#include "VirtualMerging.h"

class VirtualMergingTest : public RedexTest {
 public:
  void SetUp() override {
    // Hierarchy:
    //                          LA;
    //        LA1;              LA2;               LA3;
    // LA11; LA12; LA13;  LA21; LA22; LA23;  LA31; LA32; LA33;

    auto make_class = [&](size_t idx,
                          const DexClass* super_class,
                          const char* name,
                          int32_t foo_val,
                          int32_t bar_val) {
      const auto type = DexType::make_type(name);
      ClassCreator cls_creator(type);
      cls_creator.set_super(super_class != nullptr ? super_class->get_type()
                                                   : type::java_lang_Object());
      auto ctor = DexMethod::make_method(std::string(name) + ".<init>:()V")
                      ->make_concrete(ACC_PUBLIC, false);
      cls_creator.add_method(ctor);
      // Should add a super call here, but...

      auto make_code = [](int32_t val) {
        std::string src = R"(
            (
              (load-param-object v1)
              (const v0 X)
              (return v0)
            ))";
        src.replace(src.find('X'), 1, std::to_string(val));
        return assembler::ircode_from_string(src);
      };
      auto foo = DexMethod::make_method(std::string(name) + ".foo:()I")
                     ->make_concrete(
                         ACC_PUBLIC, make_code(foo_val), /*is_virtual=*/true);
      cls_creator.add_method(foo);
      auto bar = DexMethod::make_method(std::string(name) + ".bar:()I")
                     ->make_concrete(
                         ACC_PUBLIC, make_code(bar_val), /*is_virtual=*/true);
      cls_creator.add_method(bar);

      DexClass* res = cls_creator.create();
      if (super_class != nullptr) {
        subtypes[super_class].push_back(res);
      }
      auto it = types.emplace(idx, res);
      redex_assert(it.second);
      return res;
    };

    auto a = make_class(0, nullptr, "LA;", 0, 0);

    auto a1 = make_class(1, a, "LA1;", 1, -1);
    /* auto a11 = */ make_class(11, a1, "LA11;", 11, -11);
    /* auto a12 = */ make_class(12, a1, "LA12;", 12, -12);
    /* auto a13 = */ make_class(13, a1, "LA13;", 13, -13);

    auto a2 = make_class(2, a, "LA2;", 2, -2);
    /* auto a21 = */ make_class(21, a2, "LA21;", 21, -21);
    /* auto a22 = */ make_class(22, a2, "LA22;", 22, -22);
    /* auto a23 = */ make_class(23, a2, "LA23;", 23, -23);

    auto a3 = make_class(3, a, "LA3;", 3, -3);
    /* auto a31 = */ make_class(31, a3, "LA31;", 31, -31);
    /* auto a32 = */ make_class(32, a3, "LA32;", 32, -32);
    /* auto a33 = */ make_class(33, a3, "LA33;", 33, -33);

    stores = DexStoresVector{};
    stores.emplace_back("store");
    // Need to be in one vector to be "same dex.""
    std::vector<DexClass*> tmp;
    std::transform(
        types.begin(), types.end(), std::back_inserter(tmp), [](const auto& p) {
          return const_cast<DexClass*>(p.second);
        });
    stores[0].add_classes(tmp);
  }

  const DexMethod* get_method(size_t idx, const char* name) {
    for (auto m : types.at(idx)->get_vmethods()) {
      if (m->get_name()->str() == name) {
        return m;
      }
    }
    return nullptr;
  }

  void post_process(DexMethod* m) {
    // Run RegAlloc to get registers somewhat under control.
    regalloc::graph_coloring::allocate({}, m);

    // Strip debug info.
    StripDebugInfoPass::Config cf;
    cf.drop_all_dbg_info = true;
    strip_debug_info_impl::StripDebugInfo sdi(cf);
    sdi.run(*m->get_code());
  }

 protected:
  std::unordered_map<size_t, const DexClass*> types;
  std::unordered_map<const DexClass*, std::vector<const DexClass*>> subtypes;

  DexStoresVector stores;
};

TEST_F(VirtualMergingTest, MergedFooNoProfiles) {
  auto scope = build_class_scope(stores);

  api::LevelChecker::init(19, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);

  std::unordered_map<const DexMethodRef*, method_profiles::Stats> profile_data;
  auto make_call_count_stat = [](double call_count) {
    method_profiles::Stats stats{};
    stats.call_count = call_count;
    return stats;
  };
  // Normal order 3->2->1, reorder to 2->1->3.
  profile_data.emplace(get_method(23, "foo"), make_call_count_stat(100));
  profile_data.emplace(get_method(21, "foo"), make_call_count_stat(50));
  profile_data.emplace(get_method(1, "foo"), make_call_count_stat(100));

  VirtualMerging vm{stores, inliner_config, 100};
  vm.run(method_profiles::MethodProfiles::initialize(
             method_profiles::COLD_START, std::move(profile_data)),
         VirtualMerging::Strategy::kLexicographical);

  auto a_foo = get_method(0, "foo");
  ASSERT_NE(nullptr, a_foo);

  post_process(const_cast<DexMethod*>(a_foo));

  std::string out_str = assembler::to_string(a_foo->get_code());

  auto expected = R"(
    (
      (load-param-object v1)

      (instance-of v1 "LA3;")
      (move-result-pseudo v0)
      (if-nez v0 :L10)

      (instance-of v1 "LA2;")
      (move-result-pseudo v0)
      (if-nez v0 :L5)

      (instance-of v1 "LA1;")
      (move-result-pseudo v0)
      (if-nez v0 :L0)

      (const v0 0)
      (return v0)

        (:L0)
        (check-cast v1 "LA1;")
        (move-result-pseudo-object v1)

        (instance-of v1 "LA13;")
        (move-result-pseudo v0)
        (if-nez v0 :L4)

        (instance-of v1 "LA12;")
        (move-result-pseudo v0)
        (if-nez v0 :L3)

        (instance-of v1 "LA11;")
        (move-result-pseudo v0)
        (if-nez v0 :L2)

        (const v0 1)
        (:L1)
        (return v0)

          (:L2)
          (check-cast v1 "LA11;")
          (move-result-pseudo-object v1)
          (const v0 11)
          (goto :L1)

          (:L3)
          (check-cast v1 "LA12;")
          (move-result-pseudo-object v1)
          (const v0 12)
          (goto :L1)

          (:L4)
          (check-cast v1 "LA13;")
          (move-result-pseudo-object v1)
          (const v0 13)
          (goto :L1)

        (:L5)
        (check-cast v1 "LA2;")
        (move-result-pseudo-object v1)

        (instance-of v1 "LA23;")
        (move-result-pseudo v0)
        (if-nez v0 :L9)

        (instance-of v1 "LA22;")
        (move-result-pseudo v0)
        (if-nez v0 :L8)

        (instance-of v1 "LA21;")
        (move-result-pseudo v0)
        (if-nez v0 :L7)

        (const v0 2)
        (:L6)
        (return v0)

          (:L7)
          (check-cast v1 "LA21;")
          (move-result-pseudo-object v1)
          (const v0 21)
          (goto :L6)

          (:L8)
          (check-cast v1 "LA22;")
          (move-result-pseudo-object v1)
          (const v0 22)
          (goto :L6)

          (:L9)
          (check-cast v1 "LA23;")
          (move-result-pseudo-object v1)
          (const v0 23)
          (goto :L6)

        (:L10)
        (check-cast v1 "LA3;")
        (move-result-pseudo-object v1)

        (instance-of v1 "LA33;")
        (move-result-pseudo v0)
        (if-nez v0 :L14)

        (instance-of v1 "LA32;")
        (move-result-pseudo v0)
        (if-nez v0 :L13)

        (instance-of v1 "LA31;")
        (move-result-pseudo v0)
        (if-nez v0 :L12)

        (const v0 3)
        (:L11)
        (return v0)

          (:L12)
          (check-cast v1 "LA31;")
          (move-result-pseudo-object v1)
          (const v0 31)
          (goto :L11)

          (:L13)
          (check-cast v1 "LA32;")
          (move-result-pseudo-object v1)
          (const v0 32)
          (goto :L11)

          (:L14)
          (check-cast v1 "LA33;")
          (move-result-pseudo-object v1)
          (const v0 33)
          (goto :L11)
    )
  )";
  auto exp_ir = assembler::ircode_from_string(expected);
  auto normalized = assembler::to_string(exp_ir.get());

  ASSERT_EQ(normalized, out_str);
}

TEST_F(VirtualMergingTest, MergedBarNoProfiles) {
  auto scope = build_class_scope(stores);

  api::LevelChecker::init(19, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);

  std::unordered_map<const DexMethodRef*, method_profiles::Stats> profile_data;
  auto make_call_count_stat = [](double call_count) {
    method_profiles::Stats stats{};
    stats.call_count = call_count;
    return stats;
  };
  // Normal order 3->2->1, reorder to 2->1->3.
  profile_data.emplace(get_method(23, "foo"), make_call_count_stat(100));
  profile_data.emplace(get_method(21, "foo"), make_call_count_stat(50));
  profile_data.emplace(get_method(1, "foo"), make_call_count_stat(100));

  VirtualMerging vm{stores, inliner_config, 100};
  vm.run(method_profiles::MethodProfiles::initialize(
             method_profiles::COLD_START, std::move(profile_data)),
         VirtualMerging::Strategy::kLexicographical);

  auto a_bar = get_method(0, "bar");
  ASSERT_NE(nullptr, a_bar);

  post_process(const_cast<DexMethod*>(a_bar));

  std::string out_str = assembler::to_string(a_bar->get_code());

  auto expected = R"(
    (
      (load-param-object v1)

      (instance-of v1 "LA3;")
      (move-result-pseudo v0)
      (if-nez v0 :L10)

      (instance-of v1 "LA2;")
      (move-result-pseudo v0)
      (if-nez v0 :L5)

      (instance-of v1 "LA1;")
      (move-result-pseudo v0)
      (if-nez v0 :L0)

      (const v0 0)
      (return v0)

        (:L0)
        (check-cast v1 "LA1;")
        (move-result-pseudo-object v1)

        (instance-of v1 "LA13;")
        (move-result-pseudo v0)
        (if-nez v0 :L4)

        (instance-of v1 "LA12;")
        (move-result-pseudo v0)
        (if-nez v0 :L3)

        (instance-of v1 "LA11;")
        (move-result-pseudo v0)
        (if-nez v0 :L2)

        (const v0 -1)
        (:L1)
        (return v0)

          (:L2)
          (check-cast v1 "LA11;")
          (move-result-pseudo-object v1)
          (const v0 -11)
          (goto :L1)

          (:L3)
          (check-cast v1 "LA12;")
          (move-result-pseudo-object v1)
          (const v0 -12)
          (goto :L1)

          (:L4)
          (check-cast v1 "LA13;")
          (move-result-pseudo-object v1)
          (const v0 -13)
          (goto :L1)

        (:L5)
        (check-cast v1 "LA2;")
        (move-result-pseudo-object v1)

        (instance-of v1 "LA23;")
        (move-result-pseudo v0)
        (if-nez v0 :L9)

        (instance-of v1 "LA22;")
        (move-result-pseudo v0)
        (if-nez v0 :L8)

        (instance-of v1 "LA21;")
        (move-result-pseudo v0)
        (if-nez v0 :L7)

        (const v0 -2)
        (:L6)
        (return v0)

          (:L7)
          (check-cast v1 "LA21;")
          (move-result-pseudo-object v1)
          (const v0 -21)
          (goto :L6)

          (:L8)
          (check-cast v1 "LA22;")
          (move-result-pseudo-object v1)
          (const v0 -22)
          (goto :L6)

          (:L9)
          (check-cast v1 "LA23;")
          (move-result-pseudo-object v1)
          (const v0 -23)
          (goto :L6)

        (:L10)
        (check-cast v1 "LA3;")
        (move-result-pseudo-object v1)

        (instance-of v1 "LA33;")
        (move-result-pseudo v0)
        (if-nez v0 :L14)

        (instance-of v1 "LA32;")
        (move-result-pseudo v0)
        (if-nez v0 :L13)

        (instance-of v1 "LA31;")
        (move-result-pseudo v0)
        (if-nez v0 :L12)

        (const v0 -3)
        (:L11)
        (return v0)

          (:L12)
          (check-cast v1 "LA31;")
          (move-result-pseudo-object v1)
          (const v0 -31)
          (goto :L11)

          (:L13)
          (check-cast v1 "LA32;")
          (move-result-pseudo-object v1)
          (const v0 -32)
          (goto :L11)

          (:L14)
          (check-cast v1 "LA33;")
          (move-result-pseudo-object v1)
          (const v0 -33)
          (goto :L11)
    )
  )";
  auto exp_ir = assembler::ircode_from_string(expected);
  auto normalized = assembler::to_string(exp_ir.get());

  ASSERT_EQ(normalized, out_str);
}

TEST_F(VirtualMergingTest, MergedFooProfiles) {
  auto scope = build_class_scope(stores);

  api::LevelChecker::init(19, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);

  std::unordered_map<const DexMethodRef*, method_profiles::Stats> profile_data;
  auto make_call_count_stat = [](double call_count) {
    method_profiles::Stats stats{};
    stats.call_count = call_count;
    return stats;
  };
  // Normal order 3->2->1, reorder to 2->1->3.
  profile_data.emplace(get_method(23, "foo"), make_call_count_stat(100));
  profile_data.emplace(get_method(21, "foo"), make_call_count_stat(50));
  profile_data.emplace(get_method(1, "foo"), make_call_count_stat(100));

  VirtualMerging vm{stores, inliner_config, 100};
  vm.run(method_profiles::MethodProfiles::initialize(
             method_profiles::COLD_START, std::move(profile_data)),
         VirtualMerging::Strategy::kProfileCallCount);

  auto a_foo = get_method(0, "foo");
  ASSERT_NE(nullptr, a_foo);

  post_process(const_cast<DexMethod*>(a_foo));

  std::string out_str = assembler::to_string(a_foo->get_code());

  auto expected = R"(
    (
      (load-param-object v1)

      (instance-of v1 "LA2;")
      (move-result-pseudo v0)
      (if-nez v0 :L10)

      (instance-of v1 "LA1;")
      (move-result-pseudo v0)
      (if-nez v0 :L5)

      (instance-of v1 "LA3;")
      (move-result-pseudo v0)
      (if-nez v0 :L0)

      (const v0 0)
      (return v0)

        (:L0)
        (check-cast v1 "LA3;")
        (move-result-pseudo-object v1)

        (instance-of v1 "LA33;")
        (move-result-pseudo v0)
        (if-nez v0 :L4)

        (instance-of v1 "LA32;")
        (move-result-pseudo v0)
        (if-nez v0 :L3)

        (instance-of v1 "LA31;")
        (move-result-pseudo v0)
        (if-nez v0 :L2)

        (const v0 3)
        (:L1)
        (return v0)

          (:L2)
          (check-cast v1 "LA31;")
          (move-result-pseudo-object v1)
          (const v0 31)
          (goto :L1)

          (:L3)
          (check-cast v1 "LA32;")
          (move-result-pseudo-object v1)
          (const v0 32)
          (goto :L1)

          (:L4)
          (check-cast v1 "LA33;")
          (move-result-pseudo-object v1)
          (const v0 33)
          (goto :L1)

        (:L5)
        (check-cast v1 "LA1;")
        (move-result-pseudo-object v1)

        (instance-of v1 "LA13;")
        (move-result-pseudo v0)
        (if-nez v0 :L9)

        (instance-of v1 "LA12;")
        (move-result-pseudo v0)
        (if-nez v0 :L8)

        (instance-of v1 "LA11;")
        (move-result-pseudo v0)
        (if-nez v0 :L7)

        (const v0 1)
        (:L6)
        (return v0)

          (:L7)
          (check-cast v1 "LA11;")
          (move-result-pseudo-object v1)
          (const v0 11)
          (goto :L6)

          (:L8)
          (check-cast v1 "LA12;")
          (move-result-pseudo-object v1)
          (const v0 12)
          (goto :L6)

          (:L9)
          (check-cast v1 "LA13;")
          (move-result-pseudo-object v1)
          (const v0 13)
          (goto :L6)

        (:L10)
        (check-cast v1 "LA2;")
        (move-result-pseudo-object v1)

        (instance-of v1 "LA23;")
        (move-result-pseudo v0)
        (if-nez v0 :L14)

        (instance-of v1 "LA21;")
        (move-result-pseudo v0)
        (if-nez v0 :L13)

        (instance-of v1 "LA22;")
        (move-result-pseudo v0)
        (if-nez v0 :L12)

        (const v0 2)
        (:L11)
        (return v0)

          (:L12)
          (check-cast v1 "LA22;")
          (move-result-pseudo-object v1)
          (const v0 22)
          (goto :L11)

          (:L13)
          (check-cast v1 "LA21;")
          (move-result-pseudo-object v1)
          (const v0 21)
          (goto :L11)

          (:L14)
          (check-cast v1 "LA23;")
          (move-result-pseudo-object v1)
          (const v0 23)
          (goto :L11)
    )
  )";
  auto exp_ir = assembler::ircode_from_string(expected);
  auto normalized = assembler::to_string(exp_ir.get());

  ASSERT_EQ(normalized, out_str);
}

TEST_F(VirtualMergingTest, MergedBarFooProfiles) {
  auto scope = build_class_scope(stores);

  api::LevelChecker::init(19, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);

  std::unordered_map<const DexMethodRef*, method_profiles::Stats> profile_data;
  auto make_call_count_stat = [](double call_count) {
    method_profiles::Stats stats{};
    stats.call_count = call_count;
    return stats;
  };
  // Normal order 3->2->1, reorder to 2->1->3.
  profile_data.emplace(get_method(23, "foo"), make_call_count_stat(100));
  profile_data.emplace(get_method(21, "foo"), make_call_count_stat(50));
  profile_data.emplace(get_method(1, "foo"), make_call_count_stat(100));

  VirtualMerging vm{stores, inliner_config, 100};
  vm.run(method_profiles::MethodProfiles::initialize(
             method_profiles::COLD_START, std::move(profile_data)),
         VirtualMerging::Strategy::kProfileCallCount);

  auto a_bar = get_method(0, "bar");
  ASSERT_NE(nullptr, a_bar);

  post_process(const_cast<DexMethod*>(a_bar));

  std::string out_str = assembler::to_string(a_bar->get_code());

  auto expected = R"(
    (
      (load-param-object v1)

      (instance-of v1 "LA3;")
      (move-result-pseudo v0)
      (if-nez v0 :L10)

      (instance-of v1 "LA2;")
      (move-result-pseudo v0)
      (if-nez v0 :L5)

      (instance-of v1 "LA1;")
      (move-result-pseudo v0)
      (if-nez v0 :L0)

      (const v0 0)
      (return v0)

        (:L0)
        (check-cast v1 "LA1;")
        (move-result-pseudo-object v1)

        (instance-of v1 "LA13;")
        (move-result-pseudo v0)
        (if-nez v0 :L4)

        (instance-of v1 "LA12;")
        (move-result-pseudo v0)
        (if-nez v0 :L3)

        (instance-of v1 "LA11;")
        (move-result-pseudo v0)
        (if-nez v0 :L2)

        (const v0 -1)
        (:L1)
        (return v0)

          (:L2)
          (check-cast v1 "LA11;")
          (move-result-pseudo-object v1)
          (const v0 -11)
          (goto :L1)

          (:L3)
          (check-cast v1 "LA12;")
          (move-result-pseudo-object v1)
          (const v0 -12)
          (goto :L1)

          (:L4)
          (check-cast v1 "LA13;")
          (move-result-pseudo-object v1)
          (const v0 -13)
          (goto :L1)

        (:L5)
        (check-cast v1 "LA2;")
        (move-result-pseudo-object v1)

        (instance-of v1 "LA23;")
        (move-result-pseudo v0)
        (if-nez v0 :L9)

        (instance-of v1 "LA22;")
        (move-result-pseudo v0)
        (if-nez v0 :L8)

        (instance-of v1 "LA21;")
        (move-result-pseudo v0)
        (if-nez v0 :L7)

        (const v0 -2)
        (:L6)
        (return v0)

          (:L7)
          (check-cast v1 "LA21;")
          (move-result-pseudo-object v1)
          (const v0 -21)
          (goto :L6)

          (:L8)
          (check-cast v1 "LA22;")
          (move-result-pseudo-object v1)
          (const v0 -22)
          (goto :L6)

          (:L9)
          (check-cast v1 "LA23;")
          (move-result-pseudo-object v1)
          (const v0 -23)
          (goto :L6)

        (:L10)
        (check-cast v1 "LA3;")
        (move-result-pseudo-object v1)

        (instance-of v1 "LA33;")
        (move-result-pseudo v0)
        (if-nez v0 :L14)

        (instance-of v1 "LA32;")
        (move-result-pseudo v0)
        (if-nez v0 :L13)

        (instance-of v1 "LA31;")
        (move-result-pseudo v0)
        (if-nez v0 :L12)

        (const v0 -3)
        (:L11)
        (return v0)

          (:L12)
          (check-cast v1 "LA31;")
          (move-result-pseudo-object v1)
          (const v0 -31)
          (goto :L11)

          (:L13)
          (check-cast v1 "LA32;")
          (move-result-pseudo-object v1)
          (const v0 -32)
          (goto :L11)

          (:L14)
          (check-cast v1 "LA33;")
          (move-result-pseudo-object v1)
          (const v0 -33)
          (goto :L11)
    )
  )";
  auto exp_ir = assembler::ircode_from_string(expected);
  auto normalized = assembler::to_string(exp_ir.get());

  ASSERT_EQ(normalized, out_str);
}
