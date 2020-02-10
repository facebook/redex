/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <gtest/gtest.h>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLoader.h"
#include "Resolver.h"
#include "Show.h"
#include "VerifyUtil.h"

bool class_clinit_exist(const DexClasses& classes, const char* name) {
  auto cls = find_class_named(classes, name);
  EXPECT_NE(nullptr, cls);
  auto clinit = cls->get_clinit();
  return clinit != nullptr;
}

bool has_sget(const DexClasses& classes,
              const char* class_name,
              const char* method_name) {
  auto cls = find_class_named(classes, class_name);
  EXPECT_NE(nullptr, cls);
  DexMethod* m = find_vmethod_named(*cls, method_name);
  EXPECT_NE(nullptr, m);
  DexCode* code = m->get_dex_code();
  EXPECT_NE(nullptr, code);
  for (const DexInstruction* insn : code->get_instructions()) {
    switch (insn->opcode()) {
    case DOPCODE_SGET:
    case DOPCODE_SGET_WIDE:
    case DOPCODE_SGET_OBJECT:
    case DOPCODE_SGET_BOOLEAN:
    case DOPCODE_SGET_BYTE:
    case DOPCODE_SGET_CHAR:
    case DOPCODE_SGET_SHORT:
      return true;
    default:
      break;
    }
  }
  return false;
}

TEST_F(PreVerify, ReplaceEncodableClinit) {
  // Encodable isn't here because we don't care if starts out with a <clinit> or
  // not. We only care that it's gone after FinalInlineV2

  EXPECT_TRUE(class_clinit_exist(classes, "Lredex/UnEncodable;"));
  EXPECT_TRUE(class_clinit_exist(classes, "Lredex/HasCharSequence;"));

  EXPECT_TRUE(
      has_sget(classes, "Lredex/FinalInlineV2Test;", "testFinalInline"));
}

/*
 * Ensure that we've removed the appropriate clinit and that we inlined the
 * values
 */
TEST_F(PostVerify, ReplaceEncodableClinit) {
  EXPECT_FALSE(class_clinit_exist(classes, "Lredex/Encodable;"));

  EXPECT_TRUE(class_clinit_exist(classes, "Lredex/UnEncodable;"));
  EXPECT_TRUE(class_clinit_exist(classes, "Lredex/HasCharSequence;"));

  EXPECT_FALSE(
      has_sget(classes, "Lredex/FinalInlineV2Test;", "testFinalInline"));
}
