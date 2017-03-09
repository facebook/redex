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
  auto mt = m->get_code()->get_entries();
  mt->push_back(dasm(OPCODE_CONST_16, {0_v, 0x1_L}));
  // this assignment is dead, but regalloc must still avoid having it write to
  // a live register
  mt->push_back(dasm(OPCODE_CONST_16, {1_v, 0x2_L}));
  mt->push_back(dasm(OPCODE_RETURN, {0_v}));
  m->get_code()->set_registers_size(2);
}

/*
 * Handling 2addr opcodes is tricky -- make sure we don't remap the dest/src
 * register twice.
 */
EQUIVALENCE_TEST(RegAllocTest, TwoAddr)(DexMethod* m) {
  using namespace dex_asm;
  auto mt = m->get_code()->get_entries();
  mt->push_back(dasm(OPCODE_CONST_16, {1_v, 0x1_L}));
  mt->push_back(dasm(OPCODE_CONST_16, {2_v, 0x2_L}));
  mt->push_back(dasm(OPCODE_ADD_INT_2ADDR, {1_v, 2_v}));
  mt->push_back(dasm(OPCODE_ADD_INT_2ADDR, {2_v, 1_v}));
  mt->push_back(dasm(OPCODE_RETURN, {2_v}));
  m->get_code()->set_registers_size(3);
}
