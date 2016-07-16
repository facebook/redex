/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexAsm.h"
#include "TestGenerator.h"
#include "Transform.h"
#include "RegAlloc.h"

class RegAllocTest : public EquivalenceTest {
  virtual void transform_method(DexMethod* m) {
    allocate_registers(m);
  }
};

EQUIVALENCE_TEST(RegAllocTest, DeadCodeKills)(DexMethod* m) {
  using namespace dex_asm;
  MethodTransformer mt(m);
  mt->push_back(dasm(OPCODE_CONST_16, {0_v, 0x1_L}));
  // this assignment is dead, but regalloc must still avoid having it write to
  // a live register
  mt->push_back(dasm(OPCODE_CONST_16, {1_v, 0x2_L}));
  mt->push_back(dasm(OPCODE_RETURN, {0_v}));
  m->get_code()->set_registers_size(2);
}
