/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexAsm.h"
#include "LocalDce.h"
#include "IRCode.h"
#include "TestGenerator.h"

class DceTest : public EquivalenceTest {
  void transform_method(DexMethod* m) override {
    LocalDcePass::run(m->get_code());
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
  auto mt = m->get_code();
  mt->push_back(dasm(OPCODE_CONST, {0_v, 0x1_L}));
  mt->push_back(dasm(OPCODE_RETURN, {0_v}));
  auto branch_mie = new MethodItemEntry(dasm(OPCODE_IF_EQZ, {0_v}));
  mt->push_back(*branch_mie);
  auto target = new BranchTarget(branch_mie);
  mt->push_back(target);
  mt->push_back(dasm(OPCODE_CONST, {0_v, 0x2_L}));
  m->get_code()->set_registers_size(1);
}
