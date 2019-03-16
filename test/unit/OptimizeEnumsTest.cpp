/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "EnumInSwitch.h"
#include "IRAssembler.h"

void setup() {
  ClassCreator cc(DexType::make_type("LFoo;"));
  cc.set_super(get_object_type());
  auto field =
      static_cast<DexField*>(DexField::make_field("LFoo;.table:[LBar;"));
  field->make_concrete(
      ACC_PUBLIC | ACC_STATIC,
      DexEncodedValue::zero_for_type(get_array_type(get_object_type())));
  cc.add_field(field);
  cc.create();
}

std::vector<optimize_enums::Info> find_enums(IRCode* code) {
  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  optimize_enums::Iterator fixpoint(&cfg);
  fixpoint.run(optimize_enums::Environment());
  const auto& result = fixpoint.collect();
  code->clear_cfg();
  return result;
}

TEST(OptimizeEnums, basic_neg) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (return-void)
    )
)");

  std::vector<optimize_enums::Info> infos = find_enums(code.get());

  EXPECT_EQ(0, infos.size());
}

TEST(OptimizeEnums, basic_pos) {
  g_redex = new RedexContext();
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (const v1 0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v0)
      (sparse-switch v0 (:case))

      (:case 0)
      (return-void)
    )
)");

  std::vector<optimize_enums::Info> infos = find_enums(code.get());

  EXPECT_EQ(1, infos.size());

  delete g_redex;
}

TEST(OptimizeEnums, overwritten) {
  g_redex = new RedexContext();
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (const v1 0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v0)
      (const v0 0)
      (sparse-switch v0 (:case))

      (:case 0)
      (return-void)
    )
)");

  std::vector<optimize_enums::Info> infos = find_enums(code.get());

  EXPECT_EQ(0, infos.size());

  delete g_redex;
}

TEST(OptimizeEnums, nested) {
  g_redex = new RedexContext();
  setup();

  auto code = assembler::ircode_from_string(R"(
    (
      (sget-object "LFoo;.table:[LBar;")
      (move-result-pseudo v0)
      (const v1 0)
      (invoke-virtual (v1) "LEnum;.ordinal:()I")
      (move-result v1)
      (aget v0 v1)
      (move-result-pseudo v0)
      (packed-switch v0 (:a))

      (return-void)

      (:a 1)
      (const v1 0)
      (invoke-virtual (v1) "Ljava/lang/Integer;.intValue:()I")
      (goto :x)

      (:x)
      (move-result v0)
      (packed-switch v0 (:b))

      (return-void)

      (:b 1)
      (return-void)
    )
)");

  std::vector<optimize_enums::Info> infos = find_enums(code.get());

  EXPECT_EQ(1, infos.size());

  delete g_redex;
}
