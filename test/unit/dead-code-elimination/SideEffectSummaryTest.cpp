/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SideEffectSummary.h"

#include <gtest/gtest.h>

#include "IRAssembler.h"
#include "LocalPointersAnalysis.h"
#include "RedexTest.h"

namespace ptrs = local_pointers;

class SideEffectSummaryTest : public RedexTest {};

EffectSummary analyze_code_effects(const IRCode* code) {
  EffectSummaryMap effect_summaries;
  MethodRefCache mref_cache;
  std::unordered_set<const DexMethod*> non_overridden_virtuals;

  const_cast<IRCode*>(code)->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();

  ptrs::FixpointIterator ptrs_fp_iter(cfg);
  ptrs_fp_iter.run(ptrs::Environment());
  return analyze_code_effects(effect_summaries,
                              non_overridden_virtuals,
                              ptrs_fp_iter,
                              &mref_cache,
                              code);
}

TEST_F(SideEffectSummaryTest, pure) {
  {
    auto code = assembler::ircode_from_string(R"(
      (
       (const v0 0)
       (return v0)
      )
    )");
    EXPECT_EQ(analyze_code_effects(code.get()), EffectSummary(EFF_NONE, {}));
  }

  {
    auto code = assembler::ircode_from_string(R"(
      (
       (sget "LFoo;.bar:I")
       (move-result-pseudo v0)
       (return v0)
      )
    )");
    EXPECT_EQ(analyze_code_effects(code.get()), EffectSummary(EFF_NONE, {}));
  }
}

TEST_F(SideEffectSummaryTest, modifiesParams) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (load-param-object v1)
      (const v2 0)
      (iput v2 v1 "LFoo;.bar:I")
      (return-void)
    )
  )");
  EXPECT_EQ(analyze_code_effects(code.get()), EffectSummary(EFF_NONE, {1}));
}

TEST_F(SideEffectSummaryTest, throws) {
  auto code = assembler::ircode_from_string(R"(
    (
      (new-instance "Ljava/lang/RuntimeException;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0) "Ljava/lang/RuntimeException;.<init>:()V")
      (throw v0)
    )
  )");
  EXPECT_EQ(analyze_code_effects(code.get()),
            EffectSummary(EFF_THROWS | EFF_UNKNOWN_INVOKE, {}));
}

TEST_F(SideEffectSummaryTest, locks) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (monitor-enter v0)
      (iget v0 "LFoo;.bar:I")
      (move-result-pseudo v1)
      (monitor-exit v0)
      (return v1)
    )
  )");
  EXPECT_EQ(analyze_code_effects(code.get()), EffectSummary(EFF_LOCKS, {}));
}

TEST_F(SideEffectSummaryTest, unknownWrite) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (sput v0 "LFoo;.qux:I")
      (return-void)
    )
  )");
  EXPECT_EQ(analyze_code_effects(code.get()),
            EffectSummary(EFF_WRITE_MAY_ESCAPE, {}));
}
