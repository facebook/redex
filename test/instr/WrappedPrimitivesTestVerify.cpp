/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>

#include "Show.h"
#include "verify/VerifyUtil.h"

using namespace testing;

namespace {
void dump_method(DexMethod* method) {
  method->balloon();
  method->get_code()->build_cfg();
  auto& cfg = method->get_code()->cfg();
  std::cout << show(method) << " " << show(cfg) << std::endl;
}
} // namespace

TEST_F(PostVerify, VerifyTransform) {
  auto usage_cls =
      find_class_named(classes, "Lcom/facebook/redex/WrappedPrimitives;");

  // Simple unboxing.
  {
    auto simple = find_method_named(*usage_cls, "simple");
    auto simple_str = stringify_for_comparision(simple);
    auto expected = assembler::ircode_from_string(R"((
      (load-param-object v2)
      (sget-object "Lcom/facebook/redex/AllValues;.L1:Lcom/facebook/redex/MyLong;")
      (move-result-pseudo-object v0)
      (const-wide v0 1)
      (invoke-virtual (v2 v0) "Lcom/facebook/redex/Receiver;.getLong:(J)J")
      (move-result-wide v0)
      (return-wide v0)
    ))");
    EXPECT_EQ(simple_str, assembler::to_string(expected.get()));
  }

  // Insertion of a cast to the underlying unwrapped API.
  {
    auto simple_cast = find_method_named(*usage_cls, "simpleCast");
    auto simple_cast_str = stringify_for_comparision(simple_cast);
    auto expected = assembler::ircode_from_string(R"((
      (load-param-object v2)
      (sget-object "Lcom/facebook/redex/AllValues;.L1:Lcom/facebook/redex/MyLong;")
      (move-result-pseudo-object v0)
      (const-wide v0 1)
      (check-cast v2 "Lcom/facebook/redex/Unsafe;")
      (move-result-pseudo-object v2)
      (invoke-interface (v2 v0) "Lcom/facebook/redex/Unsafe;.getLong:(J)J")
      (move-result-wide v0)
      (return-wide v0)
    ))");
    EXPECT_EQ(simple_cast_str, assembler::to_string(expected.get()));
  }

  {
    auto multiple_defs = find_method_named(*usage_cls, "multipleDefs");
    auto multiple_defs_str = stringify_for_comparision(multiple_defs);
    auto expected = assembler::ircode_from_string(R"((
      (load-param-object v5)
      (invoke-static () "Ljava/lang/System;.currentTimeMillis:()J")
      (move-result-wide v3)
      (const-wide v1 100)
      (cmp-long v0 v3 v1)
      (if-lez v0 :L1)
      (const-wide v1 1) ; inserted by the optimization pass
      (sget-object "Lcom/facebook/redex/AllValues;.L1:Lcom/facebook/redex/MyLong;")
      (move-result-pseudo-object v0)
      (:L0)
      (invoke-virtual (v5 v1) "Lcom/facebook/redex/Receiver;.getLong:(J)J")
      (move-result-wide v0)
      (return-wide v0)
      (:L1)
      (const-wide v1 2) ; inserted by the optimization pass
      (sget-object "Lcom/facebook/redex/AllValues;.L2:Lcom/facebook/redex/MyLong;")
      (move-result-pseudo-object v0)
      (goto :L0)
    ))");
    std::cerr << multiple_defs_str << std::endl;
    EXPECT_EQ(multiple_defs_str, assembler::to_string(expected.get()));
  }

  // Just for convenience, dump some methods as a much more readable CFG form.
  dump_method(find_method_named(*usage_cls, "run"));
  dump_method(find_method_named(*usage_cls, "runMonitor"));
}
