/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <string>

#include "ControlFlow.h"
#include "DexInstruction.h"
#include "IRCode.h"
#include "VerifyUtil.h"
#include "Walkers.h"

TEST_F(PreVerify, StringConcatenatorTest) {
  DexMethod* clinit = static_cast<DexMethod*>(DexMethod::get_method(
      "Lredex/test/instr/StringConcatenatorTest;.<clinit>:()V"));
  ASSERT_NE(nullptr, clinit);
  ASSERT_TRUE(clinit->is_def());
  ASSERT_NE(nullptr, clinit->get_dex_code());
  EXPECT_LT(1, clinit->get_dex_code()->size());

  DexField* field = static_cast<DexField*>(DexField::get_field(
      "Lredex/test/instr/StringConcatenatorTest;.concatenated:Ljava/lang/"
      "String;"));
  ASSERT_NE(nullptr, field);
  EXPECT_TRUE(field->is_def());

  if (field->is_concrete()) {
    EXPECT_EQ(DEVT_NULL, field->get_static_value()->evtype());
  } else {
    EXPECT_EQ(nullptr, field->get_static_value());
  }
}

TEST_F(PostVerify, StringConcatenatorTest) {
  DexMethod* clinit = static_cast<DexMethod*>(DexMethod::get_method(
      "Lredex/test/instr/StringConcatenatorTest;.<clinit>:()V"));
  ASSERT_EQ(nullptr, clinit);

  DexField* field = static_cast<DexField*>(DexField::get_field(
      "Lredex/test/instr/StringConcatenatorTest;.concatenated:Ljava/lang/"
      "String;"));
  ASSERT_NE(nullptr, field);
  EXPECT_TRUE(field->is_def());
  EXPECT_TRUE(field->is_concrete());

  DexEncodedValue* enc = field->get_static_value();
  ASSERT_NE(nullptr, enc);
  EXPECT_EQ(DEVT_STRING, enc->evtype());
  DexEncodedValueString* enc_str = static_cast<DexEncodedValueString*>(enc);
  EXPECT_EQ("prestuff", enc_str->show());
}
