/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ResourcesInliningPass.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <sstream>

#include "IRAssembler.h"
#include "JarLoader.h"
#include "RedexTest.h"
#include "Walkers.h"

class ResourcesInliningPassTest : public RedexTest {
 public:
  DexClass* class1;
  std::unordered_set<DexMethodRef*> dex_method_refs;
  Scope scope;
  UnorderedMap<uint32_t, resources::InlinableValue> inlinable_resources;

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
    const UnorderedMap<uint32_t, resources::InlinableValue>&
        inlinable_resources,
    const std::map<uint32_t, std::string>& id_to_name = {},
    const std::vector<std::string>& type_names = {},
    const std::optional<std::string>& package_name = std::nullopt) {
  walk::code(scope, [&](DexMethod*, IRCode& code) { code.build_cfg(); });

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
  for (auto& val : UnorderedIterable(transforms1)) {
    for (auto& vec : val.second) {
      auto* insn = vec.insn;
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

namespace {

std::string get_resource_name_class(const std::string& class_name,
                                    uint32_t resource_id) {
  std::ostringstream oss;
  oss << "(class (public) \"L" << class_name << ";\"\n"
      << "  (method (public) \"L" << class_name << ";.testMethod:()V\"\n"
      << "    (\n"
      << "      (load-param-object v7)\n"
      << "      (invoke-virtual (v7) "
         "\"Lcom/fb/resources/MainActivity;.getResources:()Landroid/content/"
         "res/Resources;\")\n"
      << "      (move-result-pseudo-object v0)\n"
      << "      (const v1 " << resource_id << ")\n"
      << "      (invoke-virtual (v0 v1) "
         "\"Landroid/content/res/Resources;.getResourceName:(I)Ljava/lang/"
         "String;\")\n"
      << "      (move-result-pseudo-object v2)\n"
      << "    )\n"
      << "  )\n"
      << ")";
  return oss.str();
}

} // namespace

// A resource id read out of bytecode can name any package, but type_names
// describes the application package only, so its type id is not an index that
// vector is guaranteed to hold.
TEST_F(ResourcesInliningPassTest, TestTypeIdOutOfRangeIsSkipped) {
  // Element i is type id i + 1, so this table describes type id 0x01 alone.
  const std::vector<std::string> type_names = {"bool"};
  const std::optional<std::string> package_name = "com.fb.resources";

  // 0x7f0a0001 has type id 0x0a, past the end; 0x7f000001 has type id 0, which
  // would underflow the numbering.
  for (uint32_t resource_id : {0x7f0a0001u, 0x7f000001u}) {
    const std::map<uint32_t, std::string> id_to_name = {
        {resource_id, "should_log"}};
    Scope out_of_range_scope;
    out_of_range_scope.push_back(
        assembler::class_from_string(get_resource_name_class(
            "Boo" + std::to_string(resource_id), resource_id)));
    auto transforms = build_test(out_of_range_scope, inlinable_resources,
                                 id_to_name, type_names, package_name);
    EXPECT_EQ(transforms.size(), 0) << "resource id " << resource_id;
  }

  // A type id the table does describe still transforms.
  const uint32_t in_range_id = 0x7f010001;
  const std::map<uint32_t, std::string> id_to_name = {
      {in_range_id, "should_log"}};
  Scope in_range_scope;
  in_range_scope.push_back(assembler::class_from_string(
      get_resource_name_class("Yay", in_range_id)));
  auto transforms = build_test(in_range_scope, inlinable_resources, id_to_name,
                               type_names, package_name);
  EXPECT_EQ(transforms.size(), 1);
}
