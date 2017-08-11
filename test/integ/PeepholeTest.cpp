/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "DexAsm.h"
#include "DexLoader.h"
#include "DexStore.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "PassManager.h"
#include "Peephole.h"
#include "Transform.h"

// Helper to hold a list of instructions
struct IRInstructionList {
  // No copying, move only
  IRInstructionList(const IRInstructionList&) = delete;
  IRInstructionList& operator=(const IRInstructionList&) = delete;
  IRInstructionList(IRInstructionList&&) = default;
  IRInstructionList& operator=(IRInstructionList&&) = default;

  std::vector<std::unique_ptr<IRInstruction>> instructions;

  explicit IRInstructionList(std::initializer_list<IRInstruction*> in) {
    for (IRInstruction* insn : in) {
      instructions.emplace_back(insn); // moves insn into unique_ptr
    }
  }

  explicit IRInstructionList(IRCode* mt) {
    for (auto& mie : InstructionIterable(mt)) {
      instructions.emplace_back(mie.insn); // moves insn into unique_ptr
    }
  }

  bool operator==(const IRInstructionList& rhs) const {
    return instructions.size() == rhs.instructions.size() &&
           std::equal(instructions.begin(),
                      instructions.end(),
                      rhs.instructions.begin(),
                      [](const std::unique_ptr<IRInstruction>& a,
                         const std::unique_ptr<IRInstruction>& b) {
                        return *a == *b;
                      });
  }
};

// Pretty-print instruction lists for gtest
static void PrintTo(const IRInstructionList& insn_list, std::ostream* os) {
  if (insn_list.instructions.empty()) {
    *os << "(empty)\n";
    return;
  }
  for (const auto& insn_ptr : insn_list.instructions) {
    *os << "\n\t" << show(insn_ptr.get());
  }
}

// Builds some arithmetic involving a literal instruction
// The opcode should be a literal-carrying opcode like OPCODE_ADD_INT_LIT16
// The source register is src_reg, dest register is 1
static IRInstructionList op_lit(DexOpcode opcode,
                                 int64_t literal,
                                 unsigned dst_reg = 1) {
  using namespace dex_asm;
  // note: args to dasm() go as dst, src, literal
  return IRInstructionList{
      dasm(OPCODE_CONST_16, {0_v, 42_L}),
      dasm(opcode,
           {Operand{VREG, dst_reg},
            0_v,
            Operand{LITERAL, static_cast<uint64_t>(literal)}}),
  };
}

// Builds arithmetic involving an opcode like MOVE or NEG
static IRInstructionList op_unary(DexOpcode opcode) {
  using namespace dex_asm;
  return IRInstructionList{dasm(OPCODE_CONST_16, {0_v, 42_L}),
                            dasm(opcode, {1_v, 0_v})};
}

class PeepholeTest : public ::testing::Test {
  RedexContext* saved_context = nullptr;
  ConfigFiles config;
  PeepholePass peephole_pass;
  PassManager manager;
  std::vector<DexStore> stores;
  DexClass* dex_class = nullptr;

  // add a void->void static method to our dex_class
  DexMethod* make_void_method(const char* method_name,
                              const IRInstructionList& insns) const {
    auto ret = get_void_type();
    auto args = DexTypeList::make_type_list({});
    auto proto = DexProto::make_proto(ret, args); // I()
    DexMethod* method = DexMethod::make_method(
        dex_class->get_type(), DexString::make_string(method_name), proto);
    method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    // FIXME we should determine the actual number of temp regs used from
    // the IRInstructionList
    method->set_code(std::make_unique<IRCode>(method, 0));

    // import our instructions
    auto mt = method->get_code();
    for (const auto& insn_ptr : insns.instructions) {
      mt->push_back(new IRInstruction(*insn_ptr));
    }
    return method;
  }

 public:
  PeepholeTest() : config(Json::nullValue), manager({&peephole_pass}) {
    manager.set_testing_mode();
  }

  virtual void SetUp() override {
    saved_context = g_redex;
    g_redex = new RedexContext();

    const char* dexfile = std::getenv("dexfile");
    ASSERT_NE(nullptr, dexfile);

    DexMetadata dm;
    dm.set_id("classes");
    DexStore root_store(dm);
    root_store.add_classes(load_classes_from_dex(dexfile));
    DexClasses& classes = root_store.get_dexen().back();
    stores.emplace_back(std::move(root_store));
    ASSERT_EQ(classes.size(), 1) << "Expected exactly one class in " << dexfile;
    dex_class = classes.at(0);
    ASSERT_NE(nullptr, dex_class);
  }

  virtual void TearDown() override {
    delete g_redex;
    g_redex = saved_context;
  }

  // Performs one peephole test. Applies peephole optimizations to the given
  // source instruction stream, and checks that it equals the expected result
  void test_1(const char* name,
              const IRInstructionList& src,
              const IRInstructionList& expected) {
    DexMethod* method = make_void_method(name, src);
    dex_class->add_method(method);
    manager.run_passes(stores, config);
    IRInstructionList result(method->get_code());
    EXPECT_EQ(result, expected) << " for test " << name;
    dex_class->remove_method(method);
  }

  // Perform a negative peephole test.
  // We expect to NOT modify these instructions.
  void test_1_nochange(const char* name, const IRInstructionList& src) {
    test_1(name, src, src);
  }
};

TEST_F(PeepholeTest, Arithmetic) {
  IRInstructionList move16 = op_unary(OPCODE_MOVE_16); // move v0, v1
  IRInstructionList negate = op_unary(OPCODE_NEG_INT); // neg v0, v1
  test_1("add8_0_to_move", op_lit(OPCODE_ADD_INT_LIT8, 0), move16);
  test_1("add16_0_to_move", op_lit(OPCODE_ADD_INT_LIT16, 0), move16);

  test_1("mult8_1_to_move", op_lit(OPCODE_MUL_INT_LIT8, 1), move16);
  test_1("mult16_1_to_move", op_lit(OPCODE_MUL_INT_LIT16, 1), move16);

  test_1("mult8_neg1_to_neg", op_lit(OPCODE_MUL_INT_LIT8, -1), negate);
  test_1("mult16_neg1_to_neg", op_lit(OPCODE_MUL_INT_LIT16, -1), negate);

  test_1("div8_neg1_to_neg", op_lit(OPCODE_DIV_INT_LIT8, -1), negate);
  test_1("div16_neg1_to_neg", op_lit(OPCODE_DIV_INT_LIT16, -1), negate);

  // These should result in no changes
  test_1_nochange("add8_15", op_lit(OPCODE_ADD_INT_LIT8, 15));
  test_1_nochange("add16_1", op_lit(OPCODE_ADD_INT_LIT16, 1));
  test_1_nochange("mult8_3", op_lit(OPCODE_MUL_INT_LIT8, 3));
  test_1_nochange("mult16_12", op_lit(OPCODE_MUL_INT_LIT16, 12));
}
