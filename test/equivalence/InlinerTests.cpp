/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <string>

#include "Creators.h"
#include "DexAsm.h"
#include "TestGenerator.h"
#include "Transform.h"
#include "Util.h"

class InlinerTestAliasedInputs : public EquivalenceTest {
  DexMethod* m_callee;
 public:
  virtual std::string test_name() {
    return "InlinerTestAliasedInputs";
  }

  virtual void setup(DexClass* cls) {
    auto ret = DexType::make_type("I");
    auto arg = DexType::make_type("I");
    auto args = DexTypeList::make_type_list({arg, arg});
    auto proto = DexProto::make_proto(ret, args); // I(I, I)
    m_callee =
        DexMethod::make_method(cls->get_type(),
                               DexString::make_string("callee_" + test_name()),
                               proto);
    m_callee->make_concrete(
        ACC_PUBLIC | ACC_STATIC, std::make_unique<DexCode>(), false);
    {
      using namespace dex_asm;
      MethodTransformer mt(m_callee);
      // note that this method will not behave the same way if v0 and v1 get
      // mapped to the same register
      mt->push_back(dasm(OPCODE_ADD_INT_2ADDR, {0_v, 1_v}));
      mt->push_back(dasm(OPCODE_ADD_INT_2ADDR, {1_v, 0_v}));
      mt->push_back(dasm(OPCODE_RETURN, {1_v}));
      m_callee->get_code()->set_registers_size(2);
      m_callee->get_code()->set_ins_size(2);
    }
    cls->add_method(m_callee);
  }

  virtual void build_method(DexMethod* m) {
    using namespace dex_asm;
    MethodTransformer mt(m);
    mt->push_back(dasm(OPCODE_CONST_16, {0_v, 0x1_L}));

    auto invoke = new DexOpcodeMethod(OPCODE_INVOKE_STATIC, m_callee, 0);
    invoke->set_arg_word_count(2);
    // reusing the same register for two separate arguments
    invoke->set_src(0, 0);
    invoke->set_src(1, 0);
    mt->push_back(invoke);

    m->get_code()->set_registers_size(2);
    m->get_code()->set_outs_size(2);
    mt->push_back(dasm(OPCODE_MOVE_RESULT, {1_v}));
    mt->push_back(dasm(OPCODE_RETURN, {1_v}));
  }

  virtual void transform_method(DexMethod* m) {
    InlineContext context(m, true /* use_liveness */);
    DexOpcodeMethod* mop = nullptr;
    for (const auto& insn : m->get_code()->get_instructions()) {
      if (insn->opcode() == OPCODE_INVOKE_STATIC) {
        mop = static_cast<DexOpcodeMethod*>(insn);
        assert(mop->get_method() == m_callee);
        break;
      }
    }
    MethodTransform::inline_16regs(context, m_callee, mop);
  }
};

REGISTER_TEST(InlinerTestAliasedInputs);

class InlinerTestLargeIfOffset : public EquivalenceTest {
 protected:
  DexMethod* m_callee;
 public:
  virtual void setup(DexClass* cls) {
    auto ret = DexType::make_type("V");
    auto args = DexTypeList::make_type_list({});
    auto proto = DexProto::make_proto(ret, args); // V()
    m_callee =
        DexMethod::make_method(cls->get_type(),
                               DexString::make_string("callee_" + test_name()),
                               proto);
    m_callee->make_concrete(
        ACC_PUBLIC | ACC_STATIC, std::make_unique<DexCode>(), false);
    {
      using namespace dex_asm;
      MethodTransformer mt(m_callee);
      // if-* opcodes store their jump offset as a 16-bit signed int. Let's
      // insert enough opcodes such that the offset overflows that width.
      for (size_t i = 0; i < 1 << 15; ++i) {
        mt->push_back(dasm(OPCODE_NOP));
      }
      mt->push_back(dasm(OPCODE_RETURN_VOID));
      m_callee->get_code()->set_registers_size(0);
      m_callee->get_code()->set_ins_size(0);
    }
    cls->add_method(m_callee);
  }

  virtual DexOpcode if_op() = 0;

  virtual void build_method(DexMethod* m) {
    using namespace dex_asm;
    MethodCreator mc(m);
    MethodBlock* main_block = mc.get_main_block();
    auto loc = mc.make_local(DexType::make_type("I"));
    auto addend = mc.make_local(DexType::make_type("I"));
    main_block->load_const(loc, 1);
    main_block->load_const(addend, 2);

    MethodBlock* false_block = main_block->if_testz(if_op(), loc);
    false_block->invoke(m_callee, {});
    false_block->binop_2addr(OPCODE_ADD_INT_2ADDR, loc, addend);

    main_block->binop_2addr(OPCODE_SUB_INT_2ADDR, loc, addend);
    main_block->ret(loc);
    mc.create();
  }

  virtual void transform_method(DexMethod* m) {
    DexOpcodeMethod* mop = nullptr;
    for (const auto& insn : m->get_code()->get_instructions()) {
      if (insn->opcode() == OPCODE_INVOKE_STATIC) {
        mop = static_cast<DexOpcodeMethod*>(insn);
        assert(mop->get_method() == m_callee);
        break;
      }
    }
    InlineContext context(m, true /* use_liveness */);
    MethodTransform::inline_16regs(context, m_callee, mop);
  }
};

class InlinerTestLargeIfOffsetTrueBranch : public InlinerTestLargeIfOffset {
 public:
  virtual std::string test_name() {
    return "InlinerTestLargeIfOffsetTrueBranch";
  }

  virtual DexOpcode if_op() {
    return OPCODE_IF_NEZ;
  }
};

REGISTER_TEST(InlinerTestLargeIfOffsetTrueBranch);

class InlinerTestLargeIfOffsetFalseBranch : public InlinerTestLargeIfOffset {
 public:
  virtual std::string test_name() {
    return "InlinerTestLargeIfOffsetFalseBranch";
  }

  virtual DexOpcode if_op() {
    return OPCODE_IF_EQZ;
  }
};

REGISTER_TEST(InlinerTestLargeIfOffsetFalseBranch);
