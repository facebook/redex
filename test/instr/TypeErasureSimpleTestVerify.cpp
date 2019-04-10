/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Show.h"
#include "verify/VerifyUtil.h"

TEST_F(PostVerify, MergeablesRemoval) {
  auto cls_a = find_class_named(classes, "Lcom/facebook/redextest/A;");
  auto cls_b = find_class_named(classes, "Lcom/facebook/redextest/B;");
  auto cls_c = find_class_named(classes, "Lcom/facebook/redextest/C;");
  auto cls_d = find_class_named(classes, "Lcom/facebook/redextest/D;");
  verify_type_erased(cls_a);
  verify_type_erased(cls_b);
  verify_type_erased(cls_c);
  verify_type_erased(cls_d);
}

TEST_F(PostVerify, SinkCommonCtorInvocation) {
  auto cls = find_class_named(
      classes, "Lcom/facebook/redextest/SimpleBaseShape0S0000000;");

  for (auto dm : cls->get_dmethods()) {
    if (dm->get_deobfuscated_name() !=
        "Lcom/facebook/redextest/SimpleBaseShape0S0000000;.<init>:(Ljava/lang/"
        "String;I)V")
      continue;

    int invocation_count = 0;
    auto param_insns = InstructionIterable(dm->get_code());
    for (auto param_it = param_insns.begin(); param_it != param_insns.end();
         ++param_it) {
      invocation_count += is_invoke_direct(param_it->insn->opcode()) ? 1 : 0;
    }
    EXPECT_EQ(invocation_count, 1);
  }
}
