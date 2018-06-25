/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LocalPointersAnalysis.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "IRAssembler.h"
#include "RedexTest.h"
#include "Show.h"

namespace ptrs = local_pointers;

using namespace testing;

class LocalPointersTest : public RedexTest {};

std::unordered_set<const IRInstruction*> to_set(const ptrs::PointerSet& pset) {
  if (!pset.is_value()) {
    return std::unordered_set<const IRInstruction*>{};
  }
  const auto& elems = pset.elements();
  std::unordered_set<const IRInstruction*> result(elems.begin(), elems.end());
  return result;
}

TEST_F(LocalPointersTest, domainOperations) {
  ptrs::Environment env1;
  ptrs::Environment env2;
  auto insn1 = (new IRInstruction(OPCODE_NEW_INSTANCE))
                   ->set_type(DexType::make_type("LFoo;"));
  auto insn2 = (new IRInstruction(OPCODE_NEW_INSTANCE))
                   ->set_type(DexType::make_type("LBar;"));
  auto insn3 = (new IRInstruction(OPCODE_NEW_INSTANCE))
                   ->set_type(DexType::make_type("LBaz;"));

  env1.set_nonescaping_pointer(0, insn1);
  env2.set_escaping_pointer(0, insn1);

  env1.set_nonescaping_pointer(1, insn1);
  env2.set_nonescaping_pointer(1, insn2);

  auto joined_env = env1.join(env2);

  EXPECT_EQ(joined_env.get_pointers(0).size(), 1);
  EXPECT_EQ(*joined_env.get_pointers(0).elements().begin(), insn1);
  EXPECT_EQ(joined_env.get_pointers(1).size(), 2);
  EXPECT_THAT(to_set(joined_env.get_pointers(1)),
              UnorderedElementsAre(insn1, insn2));
  EXPECT_EQ(joined_env.get_pointee(insn1),
            EscapeDomain(EscapeState::MAY_ESCAPE));
  EXPECT_EQ(joined_env.get_pointee(insn2),
            EscapeDomain(EscapeState::NOT_ESCAPED));
  EXPECT_EQ(joined_env.get_pointee(insn3), EscapeDomain(EscapeState::BOTTOM));
}

TEST_F(LocalPointersTest, simple) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param-object v0)
     (if-nez v0 :true)
     (new-instance "LFoo;")
     (move-result-pseudo-object v0)
     (:true)
     (return-void)
    )
  )");

  code->build_cfg();
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  ptrs::FixpointIterator fp_iter(cfg);
  fp_iter.run(ptrs::Environment());

  auto exit_env = fp_iter.get_exit_state_at(cfg.exit_block());
  EXPECT_EQ(exit_env.get_pointers(0).size(), 2);
  EXPECT_THAT(to_set(exit_env.get_pointers(0)),
              UnorderedElementsAre(
                  Pointee(Eq(*(IRInstruction(OPCODE_NEW_INSTANCE)
                                   .set_type(DexType::get_type("LFoo;"))))),
                  Pointee(Eq(*(
                      IRInstruction(IOPCODE_LOAD_PARAM_OBJECT).set_dest(0))))));
  for (auto insn : exit_env.get_pointers(0).elements()) {
    if (insn->opcode() == IOPCODE_LOAD_PARAM_OBJECT) {
      EXPECT_EQ(exit_env.get_pointee(insn),
                EscapeDomain(EscapeState::MAY_ESCAPE));
    } else if (insn->opcode() == OPCODE_NEW_INSTANCE) {
      EXPECT_EQ(exit_env.get_pointee(insn),
                EscapeDomain(EscapeState::NOT_ESCAPED));
    }
  }
}

TEST_F(LocalPointersTest, aliasEscape) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param-object v0)
     (load-param-object v1)
     (if-nez v0 :true)
     (new-instance "LFoo;")
     (move-result-pseudo-object v0)
     (:true)
     (move-object v1 v0)
     (sput-object v1 "LFoo;.bar:LFoo;")
     (return v0)
    )
  )");

  code->build_cfg();
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  ptrs::FixpointIterator fp_iter(cfg);
  fp_iter.run(ptrs::Environment());

  auto exit_env = fp_iter.get_exit_state_at(cfg.exit_block());
  auto returned_ptrs = exit_env.get_pointers(0);
  EXPECT_EQ(returned_ptrs.size(), 2);
  EXPECT_THAT(
      to_set(returned_ptrs),
      UnorderedElementsAre(
          Pointee(*(IRInstruction(OPCODE_NEW_INSTANCE)
                        .set_type(DexType::get_type("LFoo;")))),
          Pointee(*(IRInstruction(IOPCODE_LOAD_PARAM_OBJECT).set_dest(0)))));
  for (auto insn : returned_ptrs.elements()) {
    EXPECT_EQ(exit_env.get_pointee(insn),
              EscapeDomain(EscapeState::MAY_ESCAPE));
  }
}
