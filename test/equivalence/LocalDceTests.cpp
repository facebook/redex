/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexAsm.h"
#include "LocalDce.h"
#include "TestGenerator.h"
#include "Transform.h"

class DceTest : public EquivalenceTest {
  virtual void transform_method(DexMethod* m) {
    LocalDcePass::run(m);
  }
};

/*
 * We used to have issues with deleting a bunch of dead code at the tail end of
 * a method but leaving a lone if-* opcode behind, which would lead to
 * VerifyErrors since that opcode would attempt to jump past the end of the
 * method. This test checks that we clean up the if-* opcode as well.
 */
EQUIVALENCE_TEST(DceTest, TrailingIf)(DexMethod* m) {
  using namespace dex_asm;
  MethodTransformer mt(m);
  auto fm = mt->get_fatmethod_for_test();
  fm->push_back(*(new MethodItemEntry(dasm(OPCODE_CONST_16, {0_v, 0x1_L}))));
  fm->push_back(*(new MethodItemEntry(dasm(OPCODE_RETURN, {0_v}))));
  auto branch_mie = new MethodItemEntry(dasm(OPCODE_IF_EQZ, {0_v}));
  fm->push_back(*branch_mie);
  auto target = new BranchTarget();
  target->type = BRANCH_SIMPLE;
  target->src = branch_mie;
  fm->push_back(*(new MethodItemEntry(target)));
  fm->push_back(*(new MethodItemEntry(dasm(OPCODE_CONST_16, {0_v, 0x2_L}))));
  m->get_code()->set_registers_size(1);
}
