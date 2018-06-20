/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>
#include <iterator>

#include "Creators.h"
#include "ControlFlow.h"
#include "DedupBlocksPass.h"
#include "DexAsm.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"

struct Branch {
  MethodItemEntry* source;
  MethodItemEntry* target;
};

void run_passes(std::vector<Pass*> passes, std::vector<DexClass*> classes) {
  std::vector<DexStore> stores;
  DexMetadata dm;
  dm.set_id("classes");
  DexStore store(dm);
  store.add_classes(classes);
  stores.emplace_back(std::move(store));
  PassManager manager(passes);
  manager.set_testing_mode();

  Scope external_classes;
  Json::Value conf_obj = Json::nullValue;
  ConfigFiles dummy_config(conf_obj);
  manager.run_passes(stores, external_classes, dummy_config);
}

struct DedupBlocksTest : testing::Test {
  DexClass* m_class;
  DexTypeList* m_args;
  DexProto* m_proto;
  DexType* m_type;
  ClassCreator* m_creator;

  DedupBlocksTest() {
    g_redex = new RedexContext();
    m_args = DexTypeList::make_type_list({});
    m_proto = DexProto::make_proto(get_void_type(), m_args);
    m_type = DexType::make_type("testClass");

    m_creator = new ClassCreator(m_type);
    m_class = m_creator->get_class();
  }

  DexMethod* get_fresh_method(const std::string& name) {
    DexMethod* method = static_cast<DexMethod*>(DexMethod::make_method(
        m_type, DexString::make_string(name), m_proto));
    method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    method->set_code(std::make_unique<IRCode>(method, 1));
    m_creator->add_method(method);
    return method;
  }

  Branch create_branch(IROpcode op) {
    using namespace dex_asm;

    IRInstruction* insn = nullptr;
    if (op == OPCODE_GOTO) {
      insn = dasm(op);
    } else {
      insn = dasm(op, {0_v});
    }
    auto source = new MethodItemEntry(insn);
    auto target = new BranchTarget(source);
    return {source, new MethodItemEntry(target)};
  }

  void run_dedup_blocks() {
    std::vector<Pass*> passes = {
      new DedupBlocksPass()
    };
    std::vector<DexClass*> classes = {
      m_class
    };
    run_passes(passes, classes);
  }

  ~DedupBlocksTest() { delete g_redex; }
};

// in Code:     A B E C D          (where C == D)
// in CFG:      A -> B -> C -> E
//               \            /
//                >  --   D  >
//
// out Code:    A B E C
// out CFG:     A -> B -> C -> E
//               \       /
//                > --- >
TEST_F(DedupBlocksTest, simplestCase) {
  using namespace dex_asm;
  DexMethod* method = get_fresh_method("simplestCase");

  auto A_if_D = create_branch(OPCODE_IF_EQZ);
  auto B_goto_C = create_branch(OPCODE_GOTO);
  auto C_goto_E = create_branch(OPCODE_GOTO);
  auto D_goto_E = create_branch(OPCODE_GOTO);

  auto code = method->get_code();
  ASSERT_NE(code, nullptr);

  // A
  code->push_back(dasm(OPCODE_CONST, {0_v, 0_L}));
  code->push_back(dasm(OPCODE_MUL_INT, {0_v, 0_v, 0_v}));
  code->push_back(*A_if_D.source);

  // B
  code->push_back(dasm(OPCODE_MUL_INT, {0_v, 0_v, 0_v}));
  code->push_back(*B_goto_C.source);

  // E
  code->push_back(*C_goto_E.target);
  code->push_back(*D_goto_E.target);
  code->push_back(dasm(OPCODE_RETURN_VOID));

  // C
  code->push_back(*B_goto_C.target);
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 0_v, 0_v}));
  code->push_back(*C_goto_E.source);

  // D
  code->push_back(*A_if_D.target);
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 0_v, 0_v}));
  code->push_back(*D_goto_E.source);

  code->build_cfg(true);
  EXPECT_EQ(5, code->cfg().blocks().size());
  printf("Input cfg:\n%s\n", SHOW(code->cfg()));
  code->clear_cfg();

  run_dedup_blocks();

  code->build_cfg(true);
  printf("Result cfg:\n%s\n", SHOW(code->cfg()));
  EXPECT_EQ(4, code->cfg().blocks().size());
  code->clear_cfg();

  auto mie = code->begin();

  // A
  EXPECT_EQ(OPCODE_CONST, mie->insn->opcode());
  ++mie;
  EXPECT_EQ(OPCODE_MUL_INT, mie->insn->opcode());
  ++mie;
  EXPECT_EQ(OPCODE_IF_EQZ, mie->insn->opcode());
  auto a_c = mie;
  ++mie;

  // B
  EXPECT_EQ(OPCODE_MUL_INT, mie->insn->opcode());
  ++mie;
  EXPECT_EQ(OPCODE_GOTO, mie->insn->opcode());
  auto b_c = mie;
  ++mie;

  // E
  EXPECT_EQ(MFLOW_TARGET, mie->type);
  auto c_e = mie->target->src;
  ++mie;
  EXPECT_EQ(OPCODE_RETURN_VOID, mie->insn->opcode());
  ++mie;

  // C
  EXPECT_EQ(MFLOW_TARGET, mie->type);
  EXPECT_TRUE(mie->target->src == &*a_c || mie->target->src == &*b_c);
  ++mie;
  EXPECT_EQ(MFLOW_TARGET, mie->type);
  EXPECT_TRUE(mie->target->src == &*a_c || mie->target->src == &*b_c);
  ++mie;
  EXPECT_EQ(OPCODE_ADD_INT, mie->insn->opcode());
  ++mie;
  EXPECT_EQ(OPCODE_GOTO, mie->insn->opcode());
  EXPECT_EQ(c_e, &*mie);
  ++mie;

  // no D!
  EXPECT_EQ(code->end(), mie);
}

TEST_F(DedupBlocksTest, noDups) {
  auto str = R"(
    (
      (const v0 0)
      (if-eqz v0 :lbl)

      (const v0 1)

      (:lbl)
      (return v0)
    )
  )";

  auto method = get_fresh_method("noDups");
  auto code = assembler::ircode_from_string(str);
  method->set_code(std::move(code));

  run_dedup_blocks();

  auto expected_code = assembler::ircode_from_string(str);

  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(method->get_code()));
}

std::string cfg_str(IRCode* code) {
  code->build_cfg(true);
  const auto& cfg = code->cfg();
  auto result = show(cfg);
  code->clear_cfg();
  return result;
}

TEST_F(DedupBlocksTest, repeatedSwitchBlocks) {
  auto input_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 1)
      (packed-switch v0 (:a :b :c))
      (return v0)

      (:a 0)
      (return v0)

      (:b 1)
      (return v1)

      (:c 2)
      (return v1)
    )
  )");

  auto method = get_fresh_method("repeatedSwitchBlocks");
  method->set_code(std::move(input_code));
  auto code = method->get_code();

  run_dedup_blocks();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 1)
      (packed-switch v0 (:a :b :c))

      (:a 0)
      (return v0)

      (:c 2)
      (:b 1)
      (return v1)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(code))
      << "expected:\n"
      << cfg_str(expected_code.get()) << SHOW(expected_code) << "actual:\n"
      << cfg_str(code) << SHOW(method->get_code());
}

TEST_F(DedupBlocksTest, diffSuccessorsNoChange1) {
  auto str = R"(
    (
      (const v0 0)
      (if-eqz v0 :left)
      (goto :right)

      (:left)
      (const v1 1)
      (if-eqz v1 :left2)
      (goto :middle)

      (:right) ; same code as `:left` block but different successors
      (const v1 1)
      (if-eqz v1 :right2)
      (goto :middle)

      (:left2)
      (const v2 2)
      (goto :middle)

      (:right2)
      (const v3 3)

      (:middle)
      (return-void)
    )
  )";

  auto input_code = assembler::ircode_from_string(str);
  auto method = get_fresh_method("diffSuccessorsNoChange1");
  method->set_code(std::move(input_code));
  auto code = method->get_code();

  run_dedup_blocks();

  auto expected_code = assembler::ircode_from_string(str);

  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(code))
      << "expected:\n"
      << cfg_str(expected_code.get()) << SHOW(expected_code) << "actual:\n"
      << cfg_str(code) << SHOW(method->get_code());
}

TEST_F(DedupBlocksTest, diffSuccessorsNoChange2) {
  auto str = R"(
    (
      (const v0 0)
      (if-eqz v0 :left)
      (goto :right)

      (:left)
      (const v1 1)
      (if-eqz v1 :middle)
      (goto :left2)

      (:right) ; same code as `:left` block but different successors
      (const v1 1)
      (if-eqz v1 :middle)
      (goto :right2)

      (:left2)
      (const v2 2)
      (goto :middle)

      (:right2)
      (const v3 3)

      (:middle)
      (return-void)
    )
  )";

  auto input_code = assembler::ircode_from_string(str);
  auto method = get_fresh_method("diffSuccessorsNoChange2");
  method->set_code(std::move(input_code));
  auto code = method->get_code();

  run_dedup_blocks();

  auto expected_code = assembler::ircode_from_string(str);

  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(code))
      << "expected:\n"
      << cfg_str(expected_code.get()) << SHOW(expected_code) << "actual:\n"
      << cfg_str(code) << SHOW(method->get_code());
}

TEST_F(DedupBlocksTest, diamond) {
  auto input_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (if-eqz v0 :left)
      (goto :right)

      (:left)
      (const v1 1)
      (goto :middle)

      (:right)
      (const v1 1)

      (:middle)
      (return-void)
    )
  )");

  auto method = get_fresh_method("diamond");
  method->set_code(std::move(input_code));
  auto code = method->get_code();

  run_dedup_blocks();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (if-eqz v0 :left)

      (:left)
      (const v1 1)

      (:middle)
      (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(expected_code.get()),
            assembler::to_s_expr(code))
      << "expected:\n"
      << cfg_str(expected_code.get()) << SHOW(expected_code) << "actual:\n"
      << cfg_str(code) << SHOW(method->get_code());
}
