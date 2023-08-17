/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <atomic>
#include <memory>
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
    // Create a Test class.
    auto* self = DexType::make_type("LTest;");
    ClassCreator cc{self};

    cc.set_super(type::java_lang_Object()); // Should be Enum but does not
                                            // matter.

    for (auto& s : {"ALPHA", "BETA", "GAMMA"}) {
      cc.add_field(DexField::make_field(self, DexString::make_string(s), self)
                       ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL));
    }

    cc.add_field(DexField::make_field(self, DexString::make_string("$VALUES"),
                                      type::make_array_type(self))
                     ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL));

    cc.add_method(DexMethod::make_method("LTest;.<init>:(Ljava/lang/String;I)V")
                      ->make_concrete(ACC_PUBLIC | ACC_CONSTRUCTOR, false));

    clinit =
        DexMethod::make_method("LTest;.<clinit>:()V")
            ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_CONSTRUCTOR, false);
    cc.add_method(clinit);

    cc.create();
  }

  MethodResult run(IRCode* code) {
    cfg::ScopedCFG cfg(code);
    return RearrangeEnumClinitPass::run(clinit, code);
  }

  static std::string normalize(const std::string& in) {
    auto code = assembler::ircode_from_string(in);
    return assembler::to_string(code.get());
  }

  DexMethod* clinit;
};

TEST_F(RearrangeEnumClinitTest, Sgets) {
  auto src = R"(
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
  auto src = R"(
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

  auto dst = R"(
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

} // namespace rearrange_enum_clinit
