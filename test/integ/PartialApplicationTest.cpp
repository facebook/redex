/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "LocalDce.h"
#include "RedexTest.h"
#include "Show.h"
#include "Trace.h"
#include "Transform.h"
#include "Walkers.h"

#include "PartialApplication.h"

static IRCode* get_code(const std::string& s) {
  auto method = DexMethod::get_method(s);
  return method->as_def()->get_code();
}

class PartialApplicationTest : public RedexIntegrationTest {
 public:
  PartialApplicationTest() {
    ClassCreator creator(DexType::make_type("Ljava/lang/Integer;"));
    creator.set_super(type::java_lang_Object());
    creator.set_external();

    auto valueof = static_cast<DexMethod*>(DexMethod::make_method(
        "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;"));
    valueof->set_external();
    valueof->make_concrete(ACC_PUBLIC | ACC_STATIC, true);
    creator.add_method(valueof);

    auto value = static_cast<DexMethod*>(
        DexMethod::make_method("Ljava/lang/Integer;.intValue:()I"));
    value->set_external();
    value->make_concrete(ACC_PUBLIC, true);
    creator.add_method(value);

    creator.create();
  }
};

TEST_F(PartialApplicationTest, basic) {
  std::vector<Pass*> passes = {
      new PartialApplicationPass(),
  };

  run_passes(passes);

  // call_fooX gets foo$spa$
  auto expected_code = assembler::ircode_from_string(R"(
    (
     (.dbg DBG_SET_PROLOGUE_END)
     (.pos:dbg_0 "Lcom/facebook/redextest/PartialApplication;.call_foo4:()V" PartialApplication.java 30)
     (invoke-static () "Lcom/facebook/redextest/PartialApplication$Callees;.foo$spa$0$3b9e1bb0b5617ee4$0:()V")
     (.pos:dbg_1 "Lcom/facebook/redextest/PartialApplication;.call_foo4:()V" PartialApplication.java 31)
     (return-void)
    )
)");
  EXPECT_CODE_EQ(
      get_code("Lcom/facebook/redextest/PartialApplication;.call_foo4:()V"),
      expected_code.get());

  expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (const v2 2)
     (const v3 3)
     (const v4 4)
     (const v5 5)
     (const v6 6)
     (const v7 7)
     (invoke-static (v0 v1 v2 v3 v4 v5 v6 v7) "Lcom/facebook/redextest/PartialApplication$Callees;.foo:(IIIIIIII)V")
     (return-void)
    )
)");
  EXPECT_CODE_EQ(
      get_code("Lcom/facebook/redextest/"
               "PartialApplication$Callees;.foo$spa$0$3b9e1bb0b5617ee4$0:()V"),
      expected_code.get());

  // call_barX gets bar$spa$
  expected_code = assembler::ircode_from_string(R"(
    (
     (.dbg DBG_SET_PROLOGUE_END)
     (.pos:dbg_0 "Lcom/facebook/redextest/PartialApplication;.call_bar4:()V" PartialApplication.java 52)
     (const v2 1)
     (invoke-static (v2) "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;")
     (invoke-static () "Lcom/facebook/redextest/PartialApplication$Callees;.bar$spa$0$8477e08f7d55cc6f$0:()V")
     (.pos:dbg_1 "Lcom/facebook/redextest/PartialApplication;.call_bar4:()V" PartialApplication.java 53)
     (return-void)
    )
)");
  EXPECT_CODE_EQ(
      get_code("Lcom/facebook/redextest/PartialApplication;.call_bar4:()V"),
      expected_code.get());

  expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 65)
     (const v2 1)
     (invoke-static (v2) "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;")
     (move-result-object v3)
     (const v4 0)
     (const v5 3)
     (const v6 4)
     (const v7 5)
     (const v8 6)
     (invoke-static (v0 v1 v3 v4 v5 v6 v7 v8) "Lcom/facebook/redextest/PartialApplication$Callees;.bar:(SCLjava/lang/Integer;Ljava/lang/String;IIII)I")
     (return-void)
    )
)");
  EXPECT_CODE_EQ(
      get_code("Lcom/facebook/redextest/"
               "PartialApplication$Callees;.bar$spa$0$8477e08f7d55cc6f$0:()V"),
      expected_code.get());

  // call_bazX gets baz$spa$
  expected_code = assembler::ircode_from_string(R"(
    (
     (.dbg DBG_SET_PROLOGUE_END)
     (.pos:dbg_0 "Lcom/facebook/redextest/PartialApplication;.call_baz4:()I" PartialApplication.java 80)
     (new-instance "Lcom/facebook/redextest/PartialApplication$MoreCallees;")
     (move-result-pseudo-object v0)
     (invoke-direct (v0) "Lcom/facebook/redextest/PartialApplication$MoreCallees;.<init>:()V")
     (.pos:dbg_1 "Lcom/facebook/redextest/PartialApplication;.call_baz4:()I" PartialApplication.java 81)
     (.dbg DBG_START_LOCAL 0 "mc" "Lcom/facebook/redextest/PartialApplication$MoreCallees;")
     (const v1 103)
     (const v8 203)
     (invoke-virtual (v0 v1 v8) "Lcom/facebook/redextest/PartialApplication$MoreCallees;.baz$ipa$0$310a286dd75824f4$0:(II)I")
     (move-result v1)
     (return v1)
    )
)");
  EXPECT_CODE_EQ(
      get_code("Lcom/facebook/redextest/PartialApplication;.call_baz4:()I"),
      expected_code.get());

  expected_code = assembler::ircode_from_string(R"(
    (
     (load-param-object v7)
     (load-param v8)
     (load-param v9)
     (const v0 1111)
     (const v1 2222)
     (const v2 3333)
     (const v3 4444)
     (const v4 5555)
     (const v5 6666)
     (invoke-virtual (v7 v8 v0 v1 v2 v3 v4 v5 v9) "Lcom/facebook/redextest/PartialApplication$MoreCallees;.baz:(IIIIIIII)I")
     (move-result v6)
     (return v6)
    )
)");
  EXPECT_CODE_EQ(
      get_code(
          "Lcom/facebook/redextest/"
          "PartialApplication$MoreCallees;.baz$ipa$0$310a286dd75824f4$0:(II)I"),
      expected_code.get());
}
