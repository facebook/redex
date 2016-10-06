/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexUtil.h"
#include "Mutators.h"

namespace {
void drop_this(DexMethod* method) {
  auto const& code = method->get_code();
  if (!code) return;
  auto nregs = code->get_registers_size();
  auto nins = code->get_ins_size();
  auto const this_reg = nregs - nins;
  assert_log(nregs >= 1, "Too few regs: %s\n", SHOW(method));
  assert_log(nins >= 1, "Too few in regs: %s\n", SHOW(method));
  code->set_registers_size(nregs - 1);
  code->set_ins_size(nins - 1);
  for (auto insn : code->get_instructions()) {
    if (insn->dests_size() && !insn->dest_is_src()) {
      auto dest = insn->dest();
      assert(dest != this_reg);
      assert(!insn->dest_is_wide() || insn->dest() != (this_reg - 1));
      if (dest > this_reg) {
        insn->set_dest(dest - 1);
      }
    }
    for (unsigned i = 0; i < insn->srcs_size(); i++) {
      auto src = insn->src(i);
      assert_log(src != this_reg, "method: %s\ninsn: %s\n", SHOW(method), SHOW(insn));
      assert(!insn->src_is_wide(i) || insn->src(i) != (this_reg - 1));
      if (src > this_reg) {
        insn->set_src(i, src - 1);
      }
    }
  }
}
}

namespace mutators {

void make_static(DexMethod* method, KeepThis keep /* = Yes */) {
  auto proto = method->get_proto();
  auto params = proto->get_args()->get_type_list();
  auto clstype = method->get_class();
  if (keep == KeepThis::Yes) {
    // make `this` an explicit parameter
    params.push_front(clstype);
    auto new_args = DexTypeList::make_type_list(std::move(params));
    auto new_proto = DexProto::make_proto(proto->get_rtype(), new_args);
    DexMethodRef ref;
    ref.proto = new_proto;
    method->change(ref, true /* rename_on_collision */);
  } else {
    drop_this(method);
  }
  method->set_access(method->get_access() | ACC_STATIC);

  // changing the method proto means that we need to change its position in the
  // dmethod list
  auto cls = type_class(clstype);
  cls->remove_method(method);
  method->set_virtual(false);
  cls->add_method(method);
}

}
