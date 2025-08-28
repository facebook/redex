/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gtest/gtest.h"
#include <atomic>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include <gtest/gtest.h>

#include "RearrangeEnumClinit.h"

#include "Creators.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "MethodProfiles.h"
#include "RedexTest.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Trace.h"
#include "TypeUtil.h"
#include "VirtScopeHelper.h"
#include "VirtualRenamer.h"
#include "Walkers.h"

namespace rearrange_enum_clinit {

class RearrangeEnumClinitTest : public RedexTest {
 public:
  RearrangeEnumClinitTest() {
    auto create_test_class = [](const std::string& class_name,
                                const std::string& value_type) {
      auto* self = DexType::make_type(class_name);
      ClassCreator cc{self};

      cc.set_super(type::java_lang_Object()); // Should be Enum but does not
                                              // matter.

      for (const auto& s : {"ALPHA", "BETA", "GAMMA"}) {
        cc.add_field(DexField::make_field(self, DexString::make_string(s), self)
                         ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL));
      }

      cc.add_field(DexField::make_field(self, DexString::make_string("$VALUES"),
                                        type::make_array_type(self))
                       ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL));

      cc.add_field(DexField::make_field(self, DexString::make_string("OTHER"),
                                        type::make_array_type(self))
                       ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL));

      cc.add_method(
          DexMethod::make_method(class_name + ".<init>:(" + value_type + "I)V")
              ->make_concrete(ACC_PUBLIC | ACC_CONSTRUCTOR, false));

      auto* clinit_method =
          DexMethod::make_method(class_name + ".<clinit>:()V")
              ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_CONSTRUCTOR, false);
      cc.add_method(clinit_method);

      auto* values_method =
          DexMethod::make_method(class_name + ".values:()[" + class_name)
              ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
      cc.add_method(values_method);

      cc.create();

      return std::make_pair(clinit_method, values_method);
    };

    std::tie(clinit, values) =
        create_test_class("LTest;", "Ljava/lang/String;");
    std::tie(clinit_int, values_int) = create_test_class("LTestInt;", "I");
  }

  MethodResult run(IRCode* code, bool with_int = false) {
    cfg::ScopedCFG cfg(code);
    return RearrangeEnumClinitPass::run(with_int ? clinit_int : clinit, code);
  }

  static std::string normalize(const std::string& in) {
    auto code = assembler::ircode_from_string(in);
    return assembler::to_string(code.get());
  }

  DexMethod* clinit;
  DexMethod* values;

  DexMethod* clinit_int;
  DexMethod* values_int;
};

TEST_F(RearrangeEnumClinitTest, Sgets) {
  const auto* src = R"(
  (
    (const v4 2)
    (const v3 1)
    (const v2 0)
    (new-instance "LTest;")
    (move-result-pseudo-object v0)
    (const-string ALPHA)
    (move-result-pseudo-object v1)
    (invoke-direct (v0 v1 v2) "LTest;.<init>:(Ljava/lang/String;I)V")
    (sput-object v0 "LTest;.ALPHA:LTest;")
    (new-instance "LTest;")
    (move-result-pseudo-object v0)
    (const-string BETA)
    (move-result-pseudo-object v1)
    (invoke-direct (v0 v1 v3) "LTest;.<init>:(Ljava/lang/String;I)V")
    (sput-object v0 "LTest;.BETA:LTest;")
    (new-instance "LTest;")
    (move-result-pseudo-object v0)
    (const-string GAMMA)
    (move-result-pseudo-object v1)
    (invoke-direct (v0 v1 v4) "LTest;.<init>:(Ljava/lang/String;I)V")
    (sput-object v0 "LTest;.GAMMA:LTest;")
    (const v0 3)
    (new-array v0 "[LTest;")
    (move-result-pseudo-object v0)
    (sget-object "LTest;.ALPHA:LTest;")
    (move-result-pseudo-object v1)
    (aput-object v1 v0 v2)
    (sget-object "LTest;.BETA:LTest;")
    (move-result-pseudo-object v1)
    (aput-object v1 v0 v3)
    (sget-object "LTest;.GAMMA:LTest;")
    (move-result-pseudo-object v1)
    (aput-object v1 v0 v4)
    (sput-object v0 "LTest;.$VALUES:[LTest;")
    (return-void)
   )
    )";

  auto code = assembler::ircode_from_string(src);
  auto res = run(code.get());

  ASSERT_EQ(res, MethodResult::kFailed);
  EXPECT_EQ(normalize(src), assembler::to_string(code.get()));
}

TEST_F(RearrangeEnumClinitTest, Regs) {
  const auto* src = R"(
  (
    (const v4 2)
    (const v3 1)
    (const v2 0)
    (new-instance "LTest;")
    (move-result-pseudo-object v0)
    (const-string ALPHA)
    (move-result-pseudo-object v1)
    (invoke-direct (v0 v1 v2) "LTest;.<init>:(Ljava/lang/String;I)V")
    (sput-object v0 "LTest;.ALPHA:LTest;")
    (move-object v16 v0)
    (new-instance "LTest;")
    (move-result-pseudo-object v0)
    (const-string BETA)
    (move-result-pseudo-object v1)
    (invoke-direct (v0 v1 v3) "LTest;.<init>:(Ljava/lang/String;I)V")
    (sput-object v0 "LTest;.BETA:LTest;")
    (move-object v17 v0)
    (new-instance "LTest;")
    (move-result-pseudo-object v0)
    (const-string GAMMA)
    (move-result-pseudo-object v1)
    (invoke-direct (v0 v1 v4) "LTest;.<init>:(Ljava/lang/String;I)V")
    (sput-object v0 "LTest;.GAMMA:LTest;")
    (move-object v18 v0)
    (const v0 3)
    (new-array v0 "[LTest;")
    (move-result-pseudo-object v0)
    (move-object v1 v16)
    (aput-object v1 v0 v2)
    (move-object v1 v17)
    (aput-object v1 v0 v3)
    (move-object v1 v18)
    (aput-object v1 v0 v4)
    (sput-object v0 "LTest;.$VALUES:[LTest;")
    (return-void)
   )
    )";

  auto code = assembler::ircode_from_string(src);
  auto res = run(code.get());

  ASSERT_EQ(res, MethodResult::kChanged);

  const auto* dst = R"(
  (
    (const v19 3)
    (new-array v19 "[LTest;")
    (move-result-pseudo-object v20)
    (const v4 2)
    (const v3 1)
    (const v2 0)
    (new-instance "LTest;")
    (move-result-pseudo-object v0)
    (const-string ALPHA)
    (move-result-pseudo-object v1)
    (invoke-direct (v0 v1 v2) "LTest;.<init>:(Ljava/lang/String;I)V")
    (const v21 0)
    (aput-object v0 v20 v21)
    (sput-object v0 "LTest;.ALPHA:LTest;")
    (move-object v16 v0)
    (new-instance "LTest;")
    (move-result-pseudo-object v0)
    (const-string BETA)
    (move-result-pseudo-object v1)
    (invoke-direct (v0 v1 v3) "LTest;.<init>:(Ljava/lang/String;I)V")
    (const v21 1)
    (aput-object v0 v20 v21)
    (sput-object v0 "LTest;.BETA:LTest;")
    (move-object v17 v0)
    (new-instance "LTest;")
    (move-result-pseudo-object v0)
    (const-string GAMMA)
    (move-result-pseudo-object v1)
    (invoke-direct (v0 v1 v4) "LTest;.<init>:(Ljava/lang/String;I)V")
    (const v21 2)
    (aput-object v0 v20 v21)
    (sput-object v0 "LTest;.GAMMA:LTest;")
    (move-object v18 v0)
    (const v0 3)
    (move-object v1 v16)
    (move-object v1 v17)
    (move-object v1 v18)
    (sput-object v20 "LTest;.$VALUES:[LTest;")
    (return-void)
  )
    )";

  EXPECT_EQ(normalize(dst), assembler::to_string(code.get()));
}

// The array size may be reused, e.g., if it's the same as a value
// for the enum.
TEST_F(RearrangeEnumClinitTest, SizeReused) {
  const auto* src = R"(
  (
    (const v4 2)
    (const v3 1)
    (const v2 0)
    (new-instance "LTestInt;")
    (move-result-pseudo-object v0)
    (const v1 1)
    (invoke-direct (v0 v1 v2) "LTestInt;.<init>:(II)V")
    (sput-object v0 "LTestInt;.ALPHA:LTestInt;")
    (move-object v16 v0)
    (new-instance "LTestInt;")
    (move-result-pseudo-object v0)
    (const v1 2)
    (invoke-direct (v0 v1 v3) "LTestInt;.<init>:(II)V")
    (sput-object v0 "LTestInt;.BETA:LTestInt;")
    (move-object v17 v0)
    (new-instance "LTestInt;")
    (move-result-pseudo-object v0)
    (const v1 3)
    (invoke-direct (v0 v1 v4) "LTestInt;.<init>:(II)V")
    (sput-object v0 "LTestInt;.GAMMA:LTestInt;")
    (move-object v18 v0)
    (new-array v1 "[LTestInt;")
    (move-result-pseudo-object v0)
    (move-object v1 v16)
    (aput-object v1 v0 v2)
    (move-object v1 v17)
    (aput-object v1 v0 v3)
    (move-object v1 v18)
    (aput-object v1 v0 v4)
    (sput-object v0 "LTestInt;.$VALUES:[LTestInt;")
    (return-void)
   )
    )";

  auto code = assembler::ircode_from_string(src);
  auto res = run(code.get(), /*with_int=*/true);

  ASSERT_EQ(res, MethodResult::kChanged);

  const auto* dst = R"(
  (
    (const v19 3)
    (new-array v19 "[LTestInt;")
    (move-result-pseudo-object v20)
    (const v4 2)
    (const v3 1)
    (const v2 0)
    (new-instance "LTestInt;")
    (move-result-pseudo-object v0)
    (const v1 1)
    (invoke-direct (v0 v1 v2) "LTestInt;.<init>:(II)V")
    (const v21 0)
    (aput-object v0 v20 v21)
    (sput-object v0 "LTestInt;.ALPHA:LTestInt;")
    (move-object v16 v0)
    (new-instance "LTestInt;")
    (move-result-pseudo-object v0)
    (const v1 2)
    (invoke-direct (v0 v1 v3) "LTestInt;.<init>:(II)V")
    (const v21 1)
    (aput-object v0 v20 v21)
    (sput-object v0 "LTestInt;.BETA:LTestInt;")
    (move-object v17 v0)
    (new-instance "LTestInt;")
    (move-result-pseudo-object v0)
    (const v1 3)
    (invoke-direct (v0 v1 v4) "LTestInt;.<init>:(II)V")
    (const v21 2)
    (aput-object v0 v20 v21)
    (sput-object v0 "LTestInt;.GAMMA:LTestInt;")
    (move-object v18 v0)
    (move-object v1 v16)
    (move-object v1 v17)
    (move-object v1 v18)
    (sput-object v20 "LTestInt;.$VALUES:[LTestInt;")
    (return-void)
  )
    )";

  EXPECT_EQ(normalize(dst), assembler::to_string(code.get()));
}

// Subclass to separate things a little.
class RearrangeEnumClinitFieldTest : public RearrangeEnumClinitTest {
 protected:
  testing::AssertionResult check_other(const std::string& values_src) {
    auto values_code = assembler::ircode_from_string(values_src);
    values->set_code(std::move(values_code));

    const auto* src = R"(
    (
      (const v4 2)
      (const v3 1)
      (const v2 0)
      (new-instance "LTest;")
      (move-result-pseudo-object v0)
      (const-string ALPHA)
      (move-result-pseudo-object v1)
      (invoke-direct (v0 v1 v2) "LTest;.<init>:(Ljava/lang/String;I)V")
      (sput-object v0 "LTest;.ALPHA:LTest;")
      (move-object v16 v0)
      (new-instance "LTest;")
      (move-result-pseudo-object v0)
      (const-string BETA)
      (move-result-pseudo-object v1)
      (invoke-direct (v0 v1 v3) "LTest;.<init>:(Ljava/lang/String;I)V")
      (sput-object v0 "LTest;.BETA:LTest;")
      (move-object v17 v0)
      (new-instance "LTest;")
      (move-result-pseudo-object v0)
      (const-string GAMMA)
      (move-result-pseudo-object v1)
      (invoke-direct (v0 v1 v4) "LTest;.<init>:(Ljava/lang/String;I)V")
      (sput-object v0 "LTest;.GAMMA:LTest;")
      (move-object v18 v0)
      (const v0 3)
      (new-array v0 "[LTest;")
      (move-result-pseudo-object v0)
      (move-object v1 v16)
      (aput-object v1 v0 v2)
      (move-object v1 v17)
      (aput-object v1 v0 v3)
      (move-object v1 v18)
      (aput-object v1 v0 v4)
      (sput-object v0 "LTest;.OTHER:[LTest;")
      (return-void)
    )
      )";

    auto code = assembler::ircode_from_string(src);
    auto res = run(code.get());

    if (res != MethodResult::kChanged) {
      return testing::AssertionFailure()
             << "Optimization not applied: " << static_cast<int>(res);
    }

    const auto* dst = R"(
    (
      (const v19 3)
      (new-array v19 "[LTest;")
      (move-result-pseudo-object v20)
      (const v4 2)
      (const v3 1)
      (const v2 0)
      (new-instance "LTest;")
      (move-result-pseudo-object v0)
      (const-string ALPHA)
      (move-result-pseudo-object v1)
      (invoke-direct (v0 v1 v2) "LTest;.<init>:(Ljava/lang/String;I)V")
      (const v21 0)
      (aput-object v0 v20 v21)
      (sput-object v0 "LTest;.ALPHA:LTest;")
      (move-object v16 v0)
      (new-instance "LTest;")
      (move-result-pseudo-object v0)
      (const-string BETA)
      (move-result-pseudo-object v1)
      (invoke-direct (v0 v1 v3) "LTest;.<init>:(Ljava/lang/String;I)V")
      (const v21 1)
      (aput-object v0 v20 v21)
      (sput-object v0 "LTest;.BETA:LTest;")
      (move-object v17 v0)
      (new-instance "LTest;")
      (move-result-pseudo-object v0)
      (const-string GAMMA)
      (move-result-pseudo-object v1)
      (invoke-direct (v0 v1 v4) "LTest;.<init>:(Ljava/lang/String;I)V")
      (const v21 2)
      (aput-object v0 v20 v21)
      (sput-object v0 "LTest;.GAMMA:LTest;")
      (move-object v18 v0)
      (const v0 3)
      (move-object v1 v16)
      (move-object v1 v17)
      (move-object v1 v18)
      (sput-object v20 "LTest;.OTHER:[LTest;")
      (return-void)
    )
      )";

    auto normalized = normalize(dst);
    auto code_str = assembler::to_string(code.get());
    if (normalized != code_str) {
      return testing::AssertionFailure() << "Unexpected output:\nExpected:\n"
                                         << normalized << "\nActual:\n"
                                         << code_str;
    }
    return testing::AssertionSuccess();
  }
};

TEST_F(RearrangeEnumClinitFieldTest, OtherDirect) {
  const auto* values_src = R"(
    (
      (sget-object "LTest;.OTHER:[LTest;")
      (move-result-pseudo-object v0)
      (return-object v0)
    )
  )";
  ASSERT_TRUE(check_other(values_src));
}

TEST_F(RearrangeEnumClinitFieldTest, OtherClone) {
  const auto* values_src = R"(
    (
      (sget-object "LTest;.OTHER:[LTest;")
      (move-result-pseudo-object v0)
      (invoke-virtual (v0) "LTest;.clone:()Ljava/lang/Object;")
      (move-result-pseudo-object v0)
      (check-cast v0 "[LTest;")
      (return-object v0)
    )
  )";
  ASSERT_TRUE(check_other(values_src));
}

} // namespace rearrange_enum_clinit
