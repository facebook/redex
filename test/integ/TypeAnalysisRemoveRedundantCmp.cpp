/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "GlobalTypeAnalysisPass.h"
#include "GlobalTypeAnalyzer.h"
#include "LocalDcePass.h"
#include "Resolver.h"
#include "Show.h"
#include "TypeAnalysisTestBase.h"
#include "TypeAnalysisTransform.h"

using namespace type_analyzer;
using namespace type_analyzer::global;

class TypeAnalysisTransformTest : public TypeAnalysisTestBase {};
namespace {

TEST_F(TypeAnalysisTransformTest, MethodHasNoEqDefined) {
  auto scope = build_class_scope(stores);
  set_root_method("LTypeAnalysisRemoveRedundantCmp;.main:()V");

  auto gta = new GlobalTypeAnalysisPass();
  auto dce = new LocalDcePass();
  gta->get_config().transform.remove_redundant_null_checks = true;
  std::vector<Pass*> passes{gta, dce};
  run_passes(passes);

  auto x_method =
      DexMethod::get_method("LTypeAnalysisRemoveRedundantCmp;.getX:()I")
          ->as_def();
  auto codex = x_method->get_code();
  ASSERT_NE(nullptr, codex);
  auto ii = InstructionIterable(x_method->get_code());
  auto end = ii.end();
  for (auto it = ii.begin(); it != end; ++it) {
    auto insn = it->insn;
    EXPECT_NE(insn->opcode(), OPCODE_IF_EQZ);
    EXPECT_NE(insn->opcode(), OPCODE_IF_NEZ);
  }

  auto y_method =
      DexMethod::get_method(
          "LTypeAnalysisRemoveRedundantCmp;.getYy:()Ljava/lang/String;")
          ->as_def();
  bool found_cmpz = false;
  auto codey = y_method->get_code();
  ASSERT_NE(nullptr, codey);
  auto jj = InstructionIterable(y_method->get_code());
  auto end2 = jj.end();
  for (auto it = jj.begin(); it != end2; ++it) {
    auto insn = it->insn;
    if (insn->opcode() == OPCODE_IF_EQZ || insn->opcode() == OPCODE_IF_NEZ) {
      found_cmpz = true;
    }
  }
  EXPECT_EQ(found_cmpz, false);
}

} // namespace
