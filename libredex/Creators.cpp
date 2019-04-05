/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Creators.h"

#include <boost/range/adaptor/reversed.hpp>

#include "IROpcode.h"
#include "Transform.h"

namespace {

// TODO: make naming of methods smart
DexString* get_name(DexMethod* meth) {
  std::string name = "__st__" + meth->get_name()->str();
  return DexString::make_string(name);
}

DexProto* make_static_sig(DexMethod* meth) {
  auto proto = meth->get_proto();
  auto rtype = proto->get_rtype();
  std::deque<DexType*> arg_list;
  arg_list.push_back(meth->get_class());
  auto args = proto->get_args()->get_type_list();
  arg_list.insert(arg_list.end(), args.begin(), args.end());
  auto new_args = DexTypeList::make_type_list(std::move(arg_list));
  return DexProto::make_proto(rtype, new_args);
}

} // namespace

MethodBlock::MethodBlock(IRList::iterator iterator, MethodCreator* creator)
    : mc(creator), curr(iterator) {
  mc->blocks.push_back(this);
}

void MethodBlock::invoke(DexMethod* meth, const std::vector<Location>& args) {
  always_assert(meth->is_concrete());
  IROpcode opcode;
  if (meth->is_virtual()) {
    if (is_interface(type_class(meth->get_class()))) {
      opcode = OPCODE_INVOKE_INTERFACE;
    } else {
      opcode = OPCODE_INVOKE_VIRTUAL;
    }
  } else {
    if (is_static(meth)) {
      opcode = OPCODE_INVOKE_STATIC;
    } else {
      opcode = OPCODE_INVOKE_DIRECT;
    }
  }
  invoke(opcode, meth, args);
}

void MethodBlock::invoke(IROpcode opcode,
                         DexMethodRef* meth,
                         const std::vector<Location>& args) {
  always_assert(is_invoke(opcode));
  auto invk = new IRInstruction(opcode);
  uint16_t arg_count = static_cast<uint16_t>(args.size());
  invk->set_method(meth)->set_arg_word_count(arg_count);
  for (uint16_t i = 0; i < arg_count; i++) {
    auto arg = args.at(i);
    invk->set_src(i, arg.get_reg());
  }
  push_instruction(invk);
}

void MethodBlock::new_instance(DexType* type, Location& dst) {
  auto insn = new IRInstruction(OPCODE_NEW_INSTANCE);
  insn->set_type(type);
  push_instruction(insn);
  push_instruction((new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                       ->set_dest(dst.get_reg()));
}

void MethodBlock::new_array(DexType* type,
                            const Location& size,
                            const Location& dst) {
  auto insn = new IRInstruction(OPCODE_NEW_ARRAY);
  insn->set_type(type);
  insn->set_arg_word_count(1);
  insn->set_src(0, size.get_reg());
  push_instruction(insn);
  push_instruction((new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                       ->set_dest(dst.get_reg()));
}

void MethodBlock::throwex(Location ex) {
  auto insn = new IRInstruction(OPCODE_THROW);
  insn->set_src(0, ex.get_reg());
  push_instruction(insn);
}

void MethodBlock::iget(DexField* field, Location obj, Location& dst) {
  always_assert(field->is_concrete() && !is_static(field));
  IROpcode opcode;
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
  always_assert(field->is_concrete() && !is_static(field));
  IROpcode opcode;
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

void MethodBlock::ifield_op(IROpcode opcode,
                            DexField* field,
                            Location obj,
                            Location& src_or_dst) {
  always_assert(is_ifield_op(opcode));
  if (is_iget(opcode)) {
    auto iget = new IRInstruction(opcode);
    iget->set_field(field);
    src_or_dst.type = field->get_class();
    iget->set_src(0, obj.get_reg());
    push_instruction(iget);
    push_instruction(
        (new IRInstruction(opcode::move_result_pseudo_for_iget(opcode)))
            ->set_dest(src_or_dst.get_reg()));
  } else {
    auto iput = new IRInstruction(opcode);
    iput->set_field(field);
    iput->set_src(0, src_or_dst.get_reg());
    iput->set_src(1, obj.get_reg());
    push_instruction(iput);
  }
}

void MethodBlock::sget(DexField* field, Location& dst) {
  always_assert(field->is_concrete() && is_static(field));
  IROpcode opcode;
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
  always_assert(field->is_concrete() && is_static(field));
  IROpcode opcode;
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

void MethodBlock::sfield_op(IROpcode opcode,
                            DexField* field,
                            Location& src_or_dst) {
  always_assert(is_sfield_op(opcode));
  if (is_sget(opcode)) {
    auto sget = new IRInstruction(opcode);
    sget->set_field(field);
    src_or_dst.type = field->get_class();
    push_instruction(sget);
    push_instruction(
        (new IRInstruction(opcode::move_result_pseudo_for_sget(opcode)))
            ->set_dest(src_or_dst.get_reg()));
  } else {
    auto sput = new IRInstruction(opcode);
    sput->set_field(field)->set_src(0, src_or_dst.get_reg());
    push_instruction(sput);
  }
}

void MethodBlock::move(Location src, Location& dst) {
  always_assert(src.is_compatible(dst.type));
  auto ch = type_shorty(dst.type);
  redex_assert(ch != 'V');
  IROpcode opcode;
  if (ch == 'L')
    opcode = OPCODE_MOVE_OBJECT;
  else if (ch == 'J' || ch == 'D')
    opcode = OPCODE_MOVE_WIDE;
  else
    opcode = OPCODE_MOVE;
  IRInstruction* move = new IRInstruction(opcode);
  move->set_dest(dst.get_reg());
  move->set_src(0, src.get_reg());
  dst.type = src.type;
  push_instruction(move);
}

void MethodBlock::move_result(Location& dst, DexType* type) {
  always_assert(dst.is_compatible(type));
  auto ch = type_shorty(type);
  redex_assert(ch != 'V');
  IROpcode opcode;
  if (ch == 'L')
    opcode = OPCODE_MOVE_RESULT_OBJECT;
  else if (ch == 'J' || ch == 'D')
    opcode = OPCODE_MOVE_RESULT_WIDE;
  else
    opcode = OPCODE_MOVE_RESULT;

  IRInstruction* mov_res = new IRInstruction(opcode);
  mov_res->set_dest(dst.get_reg());
  dst.type = type;
  push_instruction(mov_res);
}

void MethodBlock::check_cast(Location& src_and_dst, DexType* type) {
  IRInstruction* check_cast = new IRInstruction(OPCODE_CHECK_CAST);
  auto reg = src_and_dst.get_reg();
  check_cast->set_type(type)->set_src(0, reg);
  push_instruction(check_cast);
  push_instruction(
      (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))->set_dest(reg));
}

void MethodBlock::instance_of(Location& obj, Location& dst, DexType* type) {
  always_assert(obj.is_ref());
  always_assert(dst.type == get_boolean_type());
  IRInstruction* insn = new IRInstruction(OPCODE_INSTANCE_OF);
  insn->set_src(0, obj.get_reg());
  insn->set_type(type);
  push_instruction(insn);
  push_instruction(
      (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO))->set_dest(dst.get_reg()));
}

void MethodBlock::ret(Location loc) {
  auto ch = type_shorty(loc.type);
  redex_assert(ch != 'V');
  IROpcode opcode;
  if (ch == 'L')
    opcode = OPCODE_RETURN_OBJECT;
  else if (ch == 'J' || ch == 'D')
    opcode = OPCODE_RETURN_WIDE;
  else
    opcode = OPCODE_RETURN;

  auto ret = new IRInstruction(opcode);
  ret->set_src(0, loc.get_reg());
  push_instruction(ret);
}

void MethodBlock::ret_void() { push_instruction(new IRInstruction(OPCODE_RETURN_VOID)); }

void MethodBlock::ret(DexType* rtype, Location loc) {
  if (rtype != get_void_type()) {
    ret(loc);
  } else {
    ret_void();
  }
}

void MethodBlock::load_const(Location& loc, int32_t value) {
  always_assert(!loc.is_wide());
  IRInstruction* load = new IRInstruction(OPCODE_CONST);
  load->set_dest(loc.get_reg());
  load->set_literal(value);
  loc.type = get_int_type();
  push_instruction(load);
}

void MethodBlock::load_const(Location& loc, double value) {
  always_assert(loc.is_wide());
  IRInstruction* load = new IRInstruction(OPCODE_CONST_WIDE);
  load->set_dest(loc.get_reg());
  load->set_literal(value);
  loc.type = get_double_type();
  push_instruction(load);
}

void MethodBlock::load_const(Location& loc, DexString* value) {
  always_assert(!loc.is_wide());
  IRInstruction* load = new IRInstruction(OPCODE_CONST_STRING);
  load->set_string(value);
  push_instruction(load);
  IRInstruction* move_result_pseudo =
      new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
  loc.type = get_string_type();
  move_result_pseudo->set_dest(loc.get_reg());
  push_instruction(move_result_pseudo);
}

void MethodBlock::load_const(Location& loc, DexType* value) {
  always_assert(!loc.is_wide());
  IRInstruction* load = new IRInstruction(OPCODE_CONST_CLASS);
  load->set_type(value);
  push_instruction(load);
  IRInstruction* move_result_pseudo =
      new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
  loc.type = get_class_type();
  move_result_pseudo->set_dest(loc.get_reg());
  push_instruction(move_result_pseudo);
}

void MethodBlock::load_null(Location& loc) {
  always_assert(!loc.is_wide());
  IRInstruction* load = new IRInstruction(OPCODE_CONST);
  load->set_dest(loc.get_reg());
  load->set_literal(0);
  loc.type = get_object_type();
  push_instruction(load);
}

void MethodBlock::init_loc(Location& loc) {
  if (loc.is_ref()) {
    load_null(loc);
  } else if (loc.is_wide()) {
    load_const(loc, 0.0);
  } else {
    load_const(loc, 0);
  }
}

void MethodBlock::binop_lit16(IROpcode op,
                              const Location& dest,
                              const Location& src,
                              int16_t literal) {
  always_assert(OPCODE_ADD_INT_LIT16 <= op && op <= OPCODE_XOR_INT_LIT16);
  always_assert(dest.type == src.type);
  always_assert(dest.type == get_int_type());
  IRInstruction* insn = new IRInstruction(op);
  insn->set_dest(dest.get_reg());
  insn->set_src(0, src.get_reg());
  insn->set_literal(literal);
  push_instruction(insn);
}

void MethodBlock::binop_lit8(IROpcode op,
                             const Location& dest,
                             const Location& src,
                             int8_t literal) {
  always_assert(OPCODE_ADD_INT_LIT8 <= op && op <= OPCODE_USHR_INT_LIT8);
  always_assert(dest.type == src.type);
  always_assert(dest.type == get_int_type());
  IRInstruction* insn = new IRInstruction(op);
  insn->set_dest(dest.get_reg());
  insn->set_src(0, src.get_reg());
  insn->set_literal(literal);
  push_instruction(insn);
}

MethodBlock* MethodBlock::if_test(IROpcode if_op,
                                  Location first,
                                  Location second) {
  always_assert(OPCODE_IF_EQ <= if_op && if_op <= OPCODE_IF_LE);
  IRInstruction* op = new IRInstruction(if_op);
  op->set_src(0, first.get_reg());
  op->set_src(1, second.get_reg());
  return make_if_block(op);
}

MethodBlock* MethodBlock::if_testz(IROpcode if_op, Location test) {
  always_assert(OPCODE_IF_EQZ <= if_op && if_op <= OPCODE_IF_LEZ);
  IRInstruction* op = new IRInstruction(if_op);
  op->set_src(0, test.get_reg());
  return make_if_block(op);
}

MethodBlock* MethodBlock::if_else_test(IROpcode if_op,
                                       Location first,
                                       Location second,
                                       MethodBlock** true_block) {
  always_assert(OPCODE_IF_EQ <= if_op && if_op <= OPCODE_IF_LE);
  IRInstruction* op = new IRInstruction(if_op);
  op->set_src(0, first.get_reg());
  op->set_src(1, second.get_reg());
  return make_if_else_block(op, true_block);
}

MethodBlock* MethodBlock::if_else_testz(IROpcode if_op,
                                        Location test,
                                        MethodBlock** true_block) {
  always_assert(OPCODE_IF_EQZ <= if_op && if_op <= OPCODE_IF_LEZ);
  IRInstruction* op = new IRInstruction(if_op);
  op->set_src(0, test.get_reg());
  return make_if_else_block(op, true_block);
}

MethodBlock* MethodBlock::switch_op(Location test,
                                    std::map<int, MethodBlock*>& cases) {
  auto sw_opcode = new IRInstruction(OPCODE_PACKED_SWITCH);
  sw_opcode->set_src(0, test.get_reg());
  // Convert to SwitchIndices map.
  std::map<SwitchIndices, MethodBlock*> indices_cases;
  for (auto it : cases) {
    SwitchIndices indices = {it.first};
    indices_cases[indices] = it.second;
  }
  auto mb = make_switch_block(sw_opcode, indices_cases);
  // Copy initialized case blocks back.
  for (auto it : indices_cases) {
    SwitchIndices indices = it.first;
    always_assert(indices.size());
    int idx = *indices.begin();
    cases[idx] = it.second;
  }
  return mb;
}

MethodBlock* MethodBlock::switch_op(
    Location test, std::map<SwitchIndices, MethodBlock*>& cases) {
  auto sw_opcode = new IRInstruction(OPCODE_PACKED_SWITCH);
  sw_opcode->set_src(0, test.get_reg());
  return make_switch_block(sw_opcode, cases);
}

void MethodCreator::load_locals(DexMethod* meth) {
  auto ii = InstructionIterable(
      meth->get_code()->get_param_instructions());
  auto it = ii.begin();
  if (!is_static(meth)) {
    make_local_at(meth->get_class(), it->insn->dest());
    ++it;
  }
  auto proto = meth->get_proto();
  auto args = proto->get_args();
  if (args) {
    for (auto arg : args->get_type_list()) {
      make_local_at(arg, it->insn->dest());
      ++it;
    }
  }
  always_assert(it == ii.end());
}

void MethodBlock::push_instruction(IRInstruction* insn) {
  curr = mc->push_instruction(curr, insn);
}

IRList::iterator MethodCreator::push_instruction(IRList::iterator curr,
                                                 IRInstruction* insn) {
  if (curr == meth_code->end()) {
    meth_code->push_back(insn);
    return std::prev(meth_code->end());
  } else {
    return meth_code->insert_after(curr, insn);
  }
}

MethodBlock* MethodBlock::make_if_block(IRInstruction* insn) {
  IRList::iterator false_block;
  curr = mc->make_if_block(curr, insn, &false_block);
  return new MethodBlock(false_block, mc);
}

IRList::iterator MethodCreator::make_if_block(
    IRList::iterator curr,
    IRInstruction* insn,
    IRList::iterator* false_block) {
  return meth_code->make_if_block(++curr, insn, false_block);
}

MethodBlock* MethodBlock::make_if_else_block(IRInstruction* insn,
                                             MethodBlock** true_block) {
  IRList::iterator if_it;
  IRList::iterator else_it;
  curr = mc->make_if_else_block(curr, insn, &if_it, &else_it);
  *true_block = new MethodBlock(else_it, mc);
  return new MethodBlock(if_it, mc);
}

IRList::iterator MethodCreator::make_if_else_block(
    IRList::iterator curr,
    IRInstruction* insn,
    IRList::iterator* false_block,
    IRList::iterator* true_block) {
  return meth_code->make_if_else_block(++curr, insn, false_block, true_block);
}

MethodBlock* MethodBlock::make_switch_block(
    IRInstruction* insn, std::map<SwitchIndices, MethodBlock*>& cases) {
  IRList::iterator default_it;
  std::map<SwitchIndices, IRList::iterator> mt_cases;
  for (auto cases_it : cases) {
    mt_cases[cases_it.first] = curr;
  }
  curr = mc->make_switch_block(curr, insn, &default_it, mt_cases);
  for (auto& cases_it : cases) {
    cases_it.second = new MethodBlock(mt_cases[cases_it.first], mc);
  }
  return new MethodBlock(default_it, mc);
}

IRList::iterator MethodCreator::make_switch_block(
    IRList::iterator curr,
    IRInstruction* insn,
    IRList::iterator* default_block,
    std::map<SwitchIndices, IRList::iterator>& cases) {
  return meth_code->make_switch_block(++curr, insn, default_block, cases);
}

std::vector<Location> MethodCreator::get_reg_args() {
  std::vector<Location> args;
  uint32_t args_size = method->get_proto()->get_args()->size();
  if (!is_static(method)) {
    args_size += 1;
  }
  args.insert(args.end(), locals.begin(), locals.begin() + args_size);
  return args;
}

MethodCreator::MethodCreator(DexMethod* meth)
    : method(meth)
    , meth_code(meth->get_code()) {
  always_assert_log(meth->is_concrete(),
      "Method must be concrete or use the other ctor");
  load_locals(meth);
  main_block = new MethodBlock(meth_code->main_block(), this);
}

MethodCreator::MethodCreator(DexMethodRef* ref,
                             DexAccessFlags access,
                             DexAnnotationSet* anno,
                             bool with_debug_item)
    : MethodCreator(ref->get_class(),
                    ref->get_name(),
                    ref->get_proto(),
                    access,
                    anno,
                    with_debug_item){};

MethodCreator::MethodCreator(DexType* cls,
                             DexString* name,
                             DexProto* proto,
                             DexAccessFlags access,
                             DexAnnotationSet* anno,
                             bool with_debug_item)
    : method(
          static_cast<DexMethod*>(DexMethod::make_method(cls, name, proto))) {
  always_assert_log(!method->is_concrete(), "Method already defined");
  if (anno) {
    method->attach_annotation_set(anno);
  }
  method->make_concrete(
      access, !(access & (ACC_STATIC | ACC_PRIVATE | ACC_CONSTRUCTOR)));
  method->set_deobfuscated_name(show(method));
  method->set_code(std::make_unique<IRCode>(method, 0));
  meth_code = method->get_code();
  if (with_debug_item) {
    meth_code->set_debug_item(std::make_unique<DexDebugItem>());
  }
  load_locals(method);
  main_block = new MethodBlock(meth_code->main_block(), this);
}

DexMethod* MethodCreator::create() {
  auto param_insns = meth_code->get_param_instructions();
  auto param_reg = meth_code->get_registers_size();
  transform::RegMap reg_map;
  // allocate all the params at the end of the register frame
  for (const auto& mie : boost::adaptors::reverse(param_insns)) {
    auto insn = mie.insn;
    param_reg -= insn->dest_is_wide() ? 2 : 1;
    reg_map[insn->dest()] = param_reg;
    if (insn->dest_is_wide()) {
      reg_map[insn->dest() + 1] = param_reg + 1;
    }
  }
  // now allocate the rest at the start
  size_t temp_reg{0};
  for (size_t i = 0; i < meth_code->get_registers_size(); ++i) {
    if (reg_map.find(i) != reg_map.end()) {
      continue;
    }
    reg_map[i] = temp_reg++;
  }
  always_assert(temp_reg == param_reg);
  transform::remap_registers(meth_code, reg_map);
  return method;
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
  redex_assert(!is_static(meth));
  redex_assert(!is_init(meth) && !is_clinit(meth));
  auto smeth = static_cast<DexMethod*>(
      DexMethod::make_method(target_cls->get_type(), name, proto));
  smeth->make_concrete(
      meth->get_access() | ACC_STATIC, meth->release_code(), false);
  target_cls->add_method(smeth);
  return smeth;
}
