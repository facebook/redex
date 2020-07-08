/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Mutators.h"

#include "DexUtil.h"
#include "IRCode.h"

namespace {
void drop_this(DexMethod* method) {
  auto code = method->get_code();
  if (!code) return;
  auto nregs = code->get_registers_size();
  assert_log(nregs >= 1, "Too few regs: %s\n", SHOW(method));
  code->set_registers_size(nregs - 1);
  auto params = code->get_param_instructions();
  auto ii = InstructionIterable(params);
  auto const this_reg = ii.begin()->insn->dest();
  code->remove_opcode(ii.begin().unwrap());
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->has_dest()) {
      auto dest = insn->dest();
      redex_assert(dest != this_reg);
      // Make sure the `this` register isn't the upper half of a wide pair.
      redex_assert(!insn->dest_is_wide() || insn->dest() != (this_reg - 1));
      if (dest > this_reg) {
        insn->set_dest(dest - 1);
      }
    }
    for (unsigned i = 0; i < insn->srcs_size(); i++) {
      auto src = insn->src(i);
      assert_log(
          src != this_reg, "method: %s\ninsn: %s\n", SHOW(method), SHOW(insn));
      if (!is_invoke(insn->opcode())) {
        // Make sure the `this` register isn't the upper half of a wide pair.
        // Exclude invoke because they explicitly refer to all registers, even
        // upper halves.
        assert_log(!(insn->src_is_wide(i) && insn->src(i) == this_reg - 1),
                   "method: %s\ninsn: %s\n",
                   SHOW(method),
                   SHOW(insn));
      }
      if (src > this_reg) {
        insn->set_src(i, src - 1);
      }
    }
  }
}
} // namespace

namespace mutators {

void make_static(DexMethod* method, KeepThis keep /* = Yes */) {
  auto proto = method->get_proto();
  auto params = proto->get_args()->get_type_list();
  auto cls_type = method->get_class();
  if (keep == KeepThis::Yes) {
    // make `this` an explicit parameter
    params.push_front(cls_type);
    auto new_args = DexTypeList::make_type_list(std::move(params));
    auto new_proto = DexProto::make_proto(proto->get_rtype(), new_args);
    DexMethodSpec spec;
    spec.proto = new_proto;
    method->change(spec, true /* rename on collision */);
  } else {
    drop_this(method);
  }
  method->set_access(method->get_access() | ACC_STATIC);

  // changing the method proto means that we need to change its position in the
  // dmethod list
  auto cls = type_class(cls_type);
  cls->remove_method(method);
  method->set_virtual(false);
  cls->add_method(method);
}

void make_non_static(DexMethod* method, bool make_virtual) {
  always_assert(method->get_access() & ACC_STATIC);
  auto proto = method->get_proto();
  auto params = proto->get_args()->get_type_list();
  auto cls_type = method->get_class();
  // Limitation: We can only deal with static methods that have a first
  // of the parameter class type.
  always_assert(cls_type == params.front());
  params.pop_front();
  auto new_args = DexTypeList::make_type_list(std::move(params));
  auto new_proto = DexProto::make_proto(proto->get_rtype(), new_args);
  DexMethodSpec spec;
  spec.proto = new_proto;
  method->change(spec, true /* rename on collision */);

  method->set_access(method->get_access() & ~ACC_STATIC);

  // changing the method proto means that we need to change its position in the
  // dmethod list
  auto cls = type_class(cls_type);
  cls->remove_method(method);
  if (make_virtual) {
    method->set_virtual(true);
  }
  cls->add_method(method);
}
} // namespace mutators
