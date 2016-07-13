/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Creators.h"

namespace {

// TODO: make naming of methods smart
DexString* get_name(DexMethod* meth) {
  std::string name(meth->get_name()->c_str());
  name = "__st__" + name;
  return DexString::make_string(name.c_str());
}

DexProto* make_static_sig(DexMethod* meth) {
  auto proto = meth->get_proto();
  auto rtype = proto->get_rtype();
  std::list<DexType*> arg_list;
  arg_list.push_back(meth->get_class());
  auto args = proto->get_args();
  for (auto arg : args->get_type_list()) {
    arg_list.push_back(arg);
  }
  auto new_args = DexTypeList::make_type_list(std::move(arg_list));
  return DexProto::make_proto(rtype, new_args);
}

/**
 * Return the register number that would correctly work after flipping the reg.
 * For wide registers returns the high reg.
 */
int reg_num(Location& loc) {
  auto type = loc.get_type();
  char t = type_shorty(type);
  always_assert(type != get_void_type());
  return t == 'J' || t == 'D' ? loc.get_reg() + 1 : loc.get_reg();
}
}

MethodBlock::MethodBlock(FatMethod::iterator iterator, MethodCreator* creator)
    : mc(creator), curr(iterator) {}

void MethodBlock::invoke(DexMethod* meth, std::vector<Location>& args) {
  always_assert(meth->is_concrete());
  DexOpcode opcode;
  if (meth->is_virtual()) {
    if (type_class(meth->get_class())->get_access() & ACC_INTERFACE) {
      opcode = OPCODE_INVOKE_INTERFACE;
    } else {
      opcode = OPCODE_INVOKE_VIRTUAL;
    }
  } else {
    if (meth->get_access() & ACC_STATIC) {
      opcode = OPCODE_INVOKE_STATIC;
    } else {
      opcode = OPCODE_INVOKE_DIRECT;
    }
  }
  invoke(opcode, meth, args);
}

void MethodBlock::invoke(DexOpcode opcode,
                         DexMethod* meth,
                         std::vector<Location>& args) {
  always_assert(is_invoke(opcode));
  auto invk = new DexOpcodeMethod(opcode, meth, 0);
  uint16_t arg_count = static_cast<uint16_t>(args.size());
  invk->set_arg_word_count(arg_count);
  for (uint16_t i = 0; i < arg_count; i++) {
    auto arg = args[i];
    invk->set_src(i, reg_num(arg));
  }
  if (arg_count > mc->out_count) mc->out_count = arg_count;
  push_instruction(invk);
}

void MethodBlock::iget(DexField* field, Location obj, Location& dst) {
  always_assert(field->is_concrete() && !(field->get_access() & ACC_STATIC));
  DexOpcode opcode;
  char t = type_shorty(field->get_type());
  switch (t) {
  case 'Z':
    opcode = OPCODE_IGET_BOOLEAN;
    break;
  case 'B':
    opcode = OPCODE_IGET_BYTE;
    break;
  case 'S':
    opcode = OPCODE_IGET_SHORT;
    break;
  case 'C':
    opcode = OPCODE_IGET_CHAR;
    break;
  case 'I':
  case 'F':
    opcode = OPCODE_IGET;
    break;
  case 'J':
  case 'D':
    opcode = OPCODE_IGET_WIDE;
    break;
  case 'L':
  case '[':
    opcode = OPCODE_IGET_OBJECT;
    break;
  default:
    always_assert(false);
    break;
  }
  ifield_op(opcode, field, obj, dst);
}

void MethodBlock::iput(DexField* field, Location obj, Location src) {
  always_assert(field->is_concrete() && !(field->get_access() & ACC_STATIC));
  DexOpcode opcode;
  char t = type_shorty(field->get_type());
  switch (t) {
  case 'Z':
    opcode = OPCODE_IPUT_BOOLEAN;
    break;
  case 'B':
    opcode = OPCODE_IPUT_BYTE;
    break;
  case 'S':
    opcode = OPCODE_IPUT_SHORT;
    break;
  case 'C':
    opcode = OPCODE_IPUT_CHAR;
    break;
  case 'I':
  case 'F':
    opcode = OPCODE_IPUT;
    break;
  case 'J':
  case 'D':
    opcode = OPCODE_IPUT_WIDE;
    break;
  case 'L':
  case '[':
    opcode = OPCODE_IPUT_OBJECT;
    break;
  default:
    always_assert(false);
    break;
  }
  ifield_op(opcode, field, obj, src);
}

void MethodBlock::ifield_op(DexOpcode opcode,
                            DexField* field,
                            Location obj,
                            Location& src_or_dst) {
  always_assert(is_ifield_op(opcode));
  if (is_iget(opcode)) {
    auto iget = new DexOpcodeField(opcode, field);
    iget->set_dest(reg_num(src_or_dst));
    src_or_dst.type = field->get_class();
    iget->set_src(0, reg_num(obj));
    push_instruction(iget);
  } else {
    auto iput = new DexOpcodeField(opcode, field);
    iput->set_src(0, reg_num(src_or_dst));
    iput->set_src(1, reg_num(obj));
    push_instruction(iput);
  }
}

void MethodBlock::sget(DexField* field, Location& dst) {
  always_assert(field->is_concrete() && (field->get_access() & ACC_STATIC));
  DexOpcode opcode;
  char t = type_shorty(field->get_type());
  switch (t) {
  case 'Z':
    opcode = OPCODE_SGET_BOOLEAN;
    break;
  case 'B':
    opcode = OPCODE_SGET_BYTE;
    break;
  case 'S':
    opcode = OPCODE_SGET_SHORT;
    break;
  case 'C':
    opcode = OPCODE_SGET_CHAR;
    break;
  case 'I':
  case 'F':
    opcode = OPCODE_SGET;
    break;
  case 'J':
  case 'D':
    opcode = OPCODE_SGET_WIDE;
    break;
  case 'L':
  case '[':
    opcode = OPCODE_SGET_OBJECT;
    break;
  default:
    always_assert(false);
    break;
  }
  sfield_op(opcode, field, dst);
}

void MethodBlock::sput(DexField* field, Location src) {
  always_assert(field->is_concrete() && (field->get_access() & ACC_STATIC));
  DexOpcode opcode;
  char t = type_shorty(field->get_type());
  switch (t) {
  case 'Z':
    opcode = OPCODE_SPUT_BOOLEAN;
    break;
  case 'B':
    opcode = OPCODE_SPUT_BYTE;
    break;
  case 'S':
    opcode = OPCODE_SPUT_SHORT;
    break;
  case 'C':
    opcode = OPCODE_SPUT_CHAR;
    break;
  case 'I':
  case 'F':
    opcode = OPCODE_SPUT;
    break;
  case 'J':
  case 'D':
    opcode = OPCODE_SPUT_WIDE;
    break;
  case 'L':
  case '[':
    opcode = OPCODE_SPUT_OBJECT;
    break;
  default:
    always_assert(false);
    break;
  }
  sfield_op(opcode, field, src);
}

void MethodBlock::sfield_op(DexOpcode opcode,
                            DexField* field,
                            Location& src_or_dst) {
  always_assert(is_sfield_op(opcode));
  if (is_sget(opcode)) {
    auto sget = new DexOpcodeField(opcode, field);
    sget->set_dest(reg_num(src_or_dst));
    src_or_dst.type = field->get_class();
    push_instruction(sget);
  } else {
    auto sput = new DexOpcodeField(opcode, field);
    sput->set_src(0, reg_num(src_or_dst));
    push_instruction(sput);
  }
}

void MethodBlock::move(Location src, Location& dst) {
  always_assert(src.is_compatible(dst.type));
  auto ch = type_shorty(dst.type);
  assert(ch != 'V');
  DexOpcode opcode;
  if (ch == 'L')
    opcode = OPCODE_MOVE_OBJECT;
  else if (ch == 'J' || ch == 'D')
    opcode = OPCODE_MOVE_WIDE;
  else
    opcode = OPCODE_MOVE;
  DexInstruction* move = new DexInstruction(opcode);
  move->set_dest(reg_num(dst));
  move->set_src(0, reg_num(src));
  dst.type = src.type;
  push_instruction(move);
}

void MethodBlock::move_result(Location& dst, DexType* type) {
  always_assert(dst.is_compatible(type));
  auto ch = type_shorty(type);
  assert(ch != 'V');
  DexOpcode opcode;
  if (ch == 'L')
    opcode = OPCODE_MOVE_RESULT_OBJECT;
  else if (ch == 'J' || ch == 'D')
    opcode = OPCODE_MOVE_RESULT_WIDE;
  else
    opcode = OPCODE_MOVE_RESULT;

  DexInstruction* mov_res = new DexInstruction(opcode);
  mov_res->set_dest(reg_num(dst));
  dst.type = type;
  push_instruction(mov_res);
}

void MethodBlock::ret(Location loc) {
  auto ch = type_shorty(loc.type);
  assert(ch != 'V');
  DexOpcode opcode;
  if (ch == 'L')
    opcode = OPCODE_RETURN_OBJECT;
  else if (ch == 'J' || ch == 'D')
    opcode = OPCODE_RETURN_WIDE;
  else
    opcode = OPCODE_RETURN;

  auto ret = new DexInstruction(opcode);
  ret->set_src(0, reg_num(loc));
  push_instruction(ret);
}

void MethodBlock::ret_void() { push_instruction(new DexInstruction(OPCODE_RETURN_VOID)); }

void MethodBlock::load_const(Location& loc, int32_t value) {
  always_assert(!loc.is_wide());
  DexInstruction* load = new DexInstruction(OPCODE_CONST_16);
  load->set_dest(reg_num(loc));
  load->set_literal(value);
  loc.type = get_int_type();
  push_instruction(load);
}

void MethodBlock::load_const(Location& loc, double value) {
  always_assert(loc.is_wide());
  DexInstruction* load = new DexInstruction(OPCODE_CONST_WIDE);
  load->set_dest(reg_num(loc));
  load->set_literal(value);
  loc.type = get_double_type();
  push_instruction(load);
}

void MethodBlock::load_const(Location& loc, DexString* value) {
  always_assert(!loc.is_wide());
  DexInstruction* load = new DexOpcodeString(OPCODE_CONST_STRING, value);
  load->set_dest(reg_num(loc));
  loc.type = get_string_type();
  push_instruction(load);
}

void MethodBlock::load_const(Location& loc, DexType* value) {
  always_assert(!loc.is_wide());
  DexInstruction* load = new DexOpcodeType(OPCODE_CONST_CLASS, value);
  load->set_dest(reg_num(loc));
  loc.type = get_class_type();
  push_instruction(load);
}

void MethodBlock::load_null(Location& loc) {
  always_assert(!loc.is_wide());
  DexInstruction* load = new DexInstruction(OPCODE_CONST_4);
  load->set_dest(reg_num(loc));
  load->set_literal(0);
  loc.type = get_object_type();
  push_instruction(load);
}

MethodBlock* MethodBlock::if_test(DexOpcode if_op,
                                  Location first,
                                  Location second) {
  always_assert(OPCODE_IF_EQ <= if_op && if_op <= OPCODE_IF_LE);
  DexInstruction* op = new DexInstruction(if_op);
  op->set_src(0, reg_num(first));
  op->set_src(1, reg_num(second));
  return make_if_block(op);
}

MethodBlock* MethodBlock::if_testz(DexOpcode if_op, Location test) {
  always_assert(OPCODE_IF_EQZ <= if_op && if_op <= OPCODE_IF_LEZ);
  DexInstruction* op = new DexInstruction(if_op);
  op->set_src(0, reg_num(test));
  return make_if_block(op);
}

MethodBlock* MethodBlock::if_else_test(DexOpcode if_op,
                                       Location first,
                                       Location second,
                                       MethodBlock** true_block) {
  always_assert(OPCODE_IF_EQ <= if_op && if_op <= OPCODE_IF_LE);
  DexInstruction* op = new DexInstruction(if_op);
  op->set_src(0, reg_num(first));
  op->set_src(1, reg_num(second));
  return make_if_else_block(op, true_block);
}

MethodBlock* MethodBlock::if_else_testz(DexOpcode if_op,
                                        Location test,
                                        MethodBlock** true_block) {
  always_assert(OPCODE_IF_EQZ <= if_op && if_op <= OPCODE_IF_LEZ);
  DexInstruction* op = new DexInstruction(if_op);
  op->set_src(0, reg_num(test));
  return make_if_else_block(op, true_block);
}

MethodBlock* MethodBlock::switch_op(Location test,
                                    std::map<int, MethodBlock*>& cases) {
  auto sw_opcode = new DexInstruction(OPCODE_PACKED_SWITCH);
  sw_opcode->set_src(0, reg_num(test));
  return make_switch_block(sw_opcode, cases);
}

void MethodCreator::load_locals(DexMethod* meth) {
  if (!(access & ACC_STATIC)) {
    make_local(meth->get_class());
  }
  auto proto = meth->get_proto();
  auto args = proto->get_args();
  if (args) {
    for (auto arg : args->get_type_list()) {
      make_local(arg);
    }
  }
}

uint16_t MethodCreator::ins_count() const {
  auto proto = method->get_proto();
  auto args = proto->get_args();
  uint16_t ins =
      args == nullptr ? 0 : static_cast<uint16_t>(args->get_type_list().size());
  if (!(access & ACC_STATIC)) ins++;
  return ins;
}

void MethodBlock::push_instruction(DexInstruction* insn) {
  curr = mc->push_instruction(curr, insn);
}

FatMethod::iterator MethodCreator::push_instruction(FatMethod::iterator curr,
                                               DexInstruction* insn) {
  return meth_code->insert(++curr, insn);
}

MethodBlock* MethodBlock::make_if_block(DexInstruction* insn) {
  FatMethod::iterator false_block;
  curr = mc->make_if_block(curr, insn, &false_block);
  return new MethodBlock(false_block, mc);
}

FatMethod::iterator MethodCreator::make_if_block(
    FatMethod::iterator curr,
    DexInstruction* insn,
    FatMethod::iterator* false_block) {
  return meth_code->make_if_block(++curr, insn, false_block);
}

MethodBlock* MethodBlock::make_if_else_block(DexInstruction* insn,
                                             MethodBlock** true_block) {
  FatMethod::iterator if_it;
  FatMethod::iterator else_it;
  curr = mc->make_if_else_block(curr, insn, &if_it, &else_it);
  *true_block = new MethodBlock(else_it, mc);
  return new MethodBlock(if_it, mc);
}

FatMethod::iterator MethodCreator::make_if_else_block(
    FatMethod::iterator curr,
    DexInstruction* insn,
    FatMethod::iterator* false_block,
    FatMethod::iterator* true_block) {
  return meth_code->make_if_else_block(++curr, insn, false_block, true_block);
}

MethodBlock* MethodBlock::make_switch_block(
    DexInstruction* insn, std::map<int, MethodBlock*>& cases) {
  FatMethod::iterator default_it;
  std::map<int, FatMethod::iterator> mt_cases;
  for (auto cases_it : cases) {
    mt_cases[cases_it.first] = curr;
  }
  curr = mc->make_switch_block(curr, insn, &default_it, mt_cases);
  for (auto& cases_it : cases) {
    cases_it.second = new MethodBlock(mt_cases[cases_it.first], mc);
  }
  return new MethodBlock(default_it, mc);
}

FatMethod::iterator MethodCreator::make_switch_block(
    FatMethod::iterator curr,
    DexInstruction* insn,
    FatMethod::iterator* default_block,
    std::map<int, FatMethod::iterator>& cases) {
  return meth_code->make_switch_block(++curr, insn, default_block, cases);
}

MethodCreator::MethodCreator(DexType* cls,
                             DexString* name,
                             DexProto* proto,
                             DexAccessFlags access)
    : method(DexMethod::make_method(cls, name, proto))
    , meth_code(MethodTransform::get_new_method(method))
    , out_count(0)
    , top_reg(0)
    , access(access) {
  always_assert_log(!method->is_concrete(), "Method already defined");
  method->set_access(access);
  load_locals(method);
  main_block = new MethodBlock(meth_code->main_block(), this);
}

DexMethod* MethodCreator::create() {
  if (method->is_concrete()) {
    method->set_code(to_code());
  } else {
    method->make_concrete(access, to_code(),
        !(access & (ACC_STATIC | ACC_PRIVATE | ACC_CONSTRUCTOR)));
  }
  return method;
}

DexCode* MethodCreator::to_code() {
  DexCode* code = new DexCode();
  code->set_registers_size(top_reg);
  code->set_ins_size(ins_count());
  code->set_outs_size(out_count);
  code->set_debug_item(nullptr);
  method->set_code(code);
  for (auto& mi : *meth_code->m_fmethod) {
    if (mi.type == MFLOW_OPCODE) {
      DexInstruction* insn = mi.insn;
      if (insn->dests_size()) {
        insn->set_dest(get_real_reg_num(insn->dest()));
      }
      for (int i = 0; i < static_cast<int>(insn->srcs_size()); i++) {
        insn->set_src(i, get_real_reg_num(insn->src(i)));
      }
    }
  }
  while (!meth_code->try_sync())
    ;
  return method->get_code();
}

DexMethod* MethodCreator::make_static_from(DexMethod* meth,
                                           DexClass* target_cls) {
  auto name = get_name(meth);
  return make_static_from(name, meth, target_cls);
}

DexMethod* MethodCreator::make_static_from(DexString* name,
                                           DexMethod* meth,
                                           DexClass* target_cls) {
  auto proto = make_static_sig(meth);
  return make_static_from(name, proto, meth, target_cls);
}

DexMethod* MethodCreator::make_static_from(DexString* name,
                                           DexProto* proto,
                                           DexMethod* meth,
                                           DexClass* target_cls) {
  assert(!(meth->get_access() & ACC_STATIC));
  assert(!is_init(meth) && !is_clinit(meth));
  auto smeth = DexMethod::make_method(target_cls->get_type(), name, proto);
  smeth->make_concrete(
      meth->get_access() | ACC_STATIC, meth->get_code(), false);
  insert_sorted(target_cls->get_dmethods(), smeth, compare_dexmethods);
  meth->set_code(nullptr);
  return smeth;
}
