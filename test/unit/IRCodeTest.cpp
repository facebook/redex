/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "DexAsm.h"
#include "IRCode.h"

std::ostream& operator<<(std::ostream& os, const IRInstruction& to_show) {
  return os << show(&to_show);
}

TEST(IRCode, LoadParamInstructionsDirect) {
  using namespace dex_asm;

  g_redex = new RedexContext();

  auto method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "V", {"I"}));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto code = std::make_unique<IRCode>(method, 3);
  auto it = code->begin();
  EXPECT_EQ(*it->insn, *dasm(IOPCODE_LOAD_PARAM, {3_v}));
  ++it;
  EXPECT_EQ(it, code->end());

  delete g_redex;
}

TEST(IRCode, LoadParamInstructionsVirtual) {
  using namespace dex_asm;

  g_redex = new RedexContext();

  auto method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "V", {"I"}));
  method->make_concrete(ACC_PUBLIC, true);
  auto code = std::make_unique<IRCode>(method, 3);
  auto it = code->begin();
  EXPECT_EQ(*it->insn, *dasm(IOPCODE_LOAD_PARAM_OBJECT, {3_v}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(IOPCODE_LOAD_PARAM, {4_v}));
  ++it;
  EXPECT_EQ(it, code->end());

  delete g_redex;
}
