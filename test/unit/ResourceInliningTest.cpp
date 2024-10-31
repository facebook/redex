/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ResourcesInliningPass.h"

#include <cstdint>
#include <gtest/gtest.h>

#include "Creators.h"
#include "DexAnnotation.h"
#include "DexInstruction.h"
#include "DexPosition.h"
#include "IRAssembler.h"
#include "JarLoader.h"
#include "RedexTest.h"
#include "Show.h"
#include "Walkers.h"

class ResourcesInliningPassTest : public RedexTest {
 public:
  DexClass* class1;
  std::unordered_set<DexMethodRef*> dex_method_refs;
  Scope scope;
  std::unordered_map<uint32_t, resources::InlinableValue> inlinable_resources;

  ResourcesInliningPassTest() {
    std::string sdk_jar = android_sdk_jar_path();
    load_jar_file(DexLocation::make_location("", sdk_jar));

    class1 = assembler::class_from_string(R"(
      (class (public) "Lcom/facebook/R$bool;"
        (field (public static final) "Lcom/facebook/R$bool;.should_log:I" #123)
      )
    )");

    scope.push_back(class1);
  }
};

MethodTransformsMap build_test(
    const Scope& scope,
    const std::unordered_map<uint32_t, resources::InlinableValue>&
        inlinable_resources) {
  walk::code(scope, [&](DexMethod*, IRCode& code) { code.build_cfg(); });

  const std::map<uint32_t, std::string> id_to_name;
  const std::vector<std::string> type_names;
  const boost::optional<std::string> package_name;

  auto transforms = ResourcesInliningPass::find_transformations(
      scope, inlinable_resources, id_to_name, type_names, package_name);
  return transforms;
}

TEST_F(ResourcesInliningPassTest, TestOptimizationHappy_Sad) {
  std::string code_class = R"(
    (class (public) "LBoo;"
      (method (public) "LBoo;.testMethod:()V"
        (
          (load-param-object v7)
          (invoke-virtual (v7)
          "Lcom/fb/resources/MainActivity;.getResources:()Landroid/content/res/Resources;")
          (move-result-pseudo-object v0)
          (sget "Lcom/facebook/R$bool;.should_log:I")
          (move-result-pseudo-object v1)
          (invoke-virtual (v0 v1) "Landroid/content/res/Resources;.getBoolean:(I)Z")
          (move-result-pseudo-object v1)
        )
      )
    )
  )";

  // TEST 1: Good! Should find 1
  resources::InlinableValue inlinable_value1;
  inlinable_value1.type = android::Res_value::TYPE_INT_BOOLEAN;
  inlinable_value1.bool_value = true;
  inlinable_resources.insert({123, inlinable_value1});
  DexClass* class2 = assembler::class_from_string(code_class);
  scope.push_back(class2);
  auto transforms1 = build_test(scope, inlinable_resources);
  EXPECT_EQ(transforms1.size(), 1);
  for (auto& val : transforms1) {
    for (auto& vec : val.second) {
      auto insn = vec.insn;
      auto inlinable_data = std::get<resources::InlinableValue>(vec.inlinable);
      EXPECT_TRUE(insn->opcode() == OPCODE_INVOKE_VIRTUAL);
      EXPECT_EQ(insn->get_method(),
                DexMethod::get_method(
                    "Landroid/content/res/Resources;.getBoolean:(I)Z"));
      EXPECT_EQ(inlinable_data.bool_value, true);
      EXPECT_EQ(inlinable_data.type, android::Res_value::TYPE_INT_BOOLEAN);
    }
  }

  // TEST 2: Bad! Since no inlinable resources, should not find any
  inlinable_resources = {};
  auto transforms2 = build_test(scope, inlinable_resources);
  EXPECT_EQ(transforms2.size(), 0);
}

// TEST 3: Bad! No invoke-virtual on supported API calls
TEST_F(ResourcesInliningPassTest, TestOptimizationBad) {
  std::string code_class = R"(
    (class (public) "LBoo;"
      (method (public) "LBoo;.testMethod:()V"
        (
          (load-param-object v7)
          (invoke-virtual (v7)
          "Lcom/fb/resources/MainActivity;.getResources:()Landroid/content/res/Resources;")
          (move-result-pseudo-object v0)
          (sget "Lcom/facebook/R$bool;.should_log:I")
          (move-result-pseudo-object v1)
        )
      )
    )
  )";

  resources::InlinableValue inlinable_value1;
  inlinable_resources.insert({123, inlinable_value1});
  DexClass* class2 = assembler::class_from_string(code_class);
  scope.push_back(class2);
  auto transforms = build_test(scope, inlinable_resources);
  EXPECT_EQ(transforms.size(), 0);
}
