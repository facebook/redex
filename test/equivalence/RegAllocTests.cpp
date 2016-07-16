/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "TestGenerator.h"
#include "Transform.h"
#include "RegAlloc.h"

class RegAllocTest : public EquivalenceTest {
  virtual void transform_method(DexMethod* m) {
    allocate_registers(m);
  }
};

EQUIVALENCE_TEST(RegAllocTest, DeadCodeKills)(DexMethod* m) {
  MethodTransformer mt(m);
  auto cst1 = new DexInstruction(OPCODE_CONST_16);
  cst1->set_dest(0);
  cst1->set_literal(1);
  mt->push_back(cst1);
  auto cst2 = new DexInstruction(OPCODE_CONST_16);
  cst2->set_dest(1);
  cst2->set_literal(2);
  mt->push_back(cst2);
  auto ret = new DexInstruction(OPCODE_RETURN);
  ret->set_src(0, 0);
  mt->push_back(ret);
  m->get_code()->set_registers_size(2);
}
