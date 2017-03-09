/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RemoveBuildersHelper.h"

#include <boost/dynamic_bitset.hpp>
#include <boost/regex.hpp>

#include "DexUtil.h"
#include "Transform.h"

DexMethod* get_build_method(std::vector<DexMethod*>& vmethods) {
  static auto build = DexString::make_string("build");
  for (const auto& vmethod : vmethods) {
    if (vmethod->get_name() == build) {
      return vmethod;
    }
  }

  return nullptr;
}

bool inline_build(DexMethod* method, DexClass* builder) {
  auto& code = method->get_code();
  if (!code) {
    return false;
  }
  auto& insns = code->get_instructions();

  std::vector<std::pair<DexMethod*, DexOpcodeMethod*>> inlinables;
  DexMethod* build_method = get_build_method(builder->get_vmethods());

  for (auto const& insn : insns) {
    if (is_invoke(insn->opcode())) {
      auto invoked = static_cast<const DexOpcodeMethod*>(insn)->get_method();
      if (invoked == build_method) {
        auto mop = static_cast<DexOpcodeMethod*>(insn);
        inlinables.push_back(std::make_pair(build_method, mop));
      }
    }
  }

  // For the moment, not treating the case where we have 2 instances
  // of the same builder.
  if (inlinables.size() > 1) {
    return false;
  }

  InlineContext inline_context(method, false);
  for (auto inlinable : inlinables) {
    // TODO(emmasevastian): We will need to gate this with a check, mostly as
    //                      we loosen the build method restraints.
    if (!MethodTransform::inline_16regs(
          inline_context, inlinable.first, inlinable.second)) {
      return false;
    }
  }

  return true;
}

bool remove_builder(DexMethod* method, DexClass* builder, DexClass* buildee) {
  auto& code = method->get_code();
  if (!code) {
    return false;
  }
  auto& insns = code->get_instructions();

  static auto init = DexString::make_string("<init>");
  bool is_builder_removed = true;

  std::unordered_map<DexField*, uint16_t> field_to_register;
  std::unordered_map<uint16_t, uint16_t> old_to_new_reg;
  std::unordered_set<uint16_t> used_regs;
  std::vector<DexInstruction*> deletes;

  // TODO(emmasevastian): For now, this only works for straight-line code.
  for (auto const& insn : insns) {
    DexOpcode opcode = insn->opcode();

    if (is_branch(opcode)) {
      is_builder_removed = false;
      break;

    } else if (opcode == OPCODE_NEW_INSTANCE) {
      DexType* cls = static_cast<DexOpcodeType*>(insn)->get_type();
      if (type_class(cls) == builder) {
        deletes.push_back(insn);
      }

    } else if (is_invoke(opcode)) {
      auto invoked = static_cast<const DexOpcodeMethod*>(insn)->get_method();
      if (invoked->get_class() == builder->get_type() &&
          invoked->get_name() == init) {
        deletes.push_back(insn);

      } else if (
          invoked->get_class() == buildee->get_type() &&
          invoked->get_name() == init) {

        // Check all fields were mapped.
        for (unsigned i = 1; i < insn->srcs_size(); ++i) {
          if (old_to_new_reg.find(insn->src(i)) == old_to_new_reg.end()) {
            is_builder_removed = false;
            break;
          }
        }

        if (!is_builder_removed) {
           break;
        }

        for (unsigned i = 1; i < insn->srcs_size(); ++i) {
          insn->set_src(i, old_to_new_reg.at(insn->src(i)));
        }

        break;
      }
    } else if (is_iput(opcode)) {
      auto field = static_cast<const DexOpcodeField*>(insn)->field();
      if (field->get_class() == builder->get_type()) {
        uint16_t used_reg = insn->src(0);
        if (used_regs.find(used_reg) != used_regs.end()) {
          is_builder_removed = false;
          break;
        }

        if (field_to_register.find(field) == field_to_register.end()) {
          field_to_register.emplace(field, insn->src(0));
        }

        used_regs.emplace(used_reg);
        deletes.push_back(insn);
      }
    } else if (is_iget(opcode)) {
      auto field = static_cast<const DexOpcodeField*>(insn)->field();
      if (field->get_class() == builder->get_type()) {
        if (field_to_register.find(field) == field_to_register.end()) {
          // Accessing an unset field.
          // TODO(emmasevastian): treat this case.
          is_builder_removed = false;
          break;
        } else {
          uint16_t stored_reg = field_to_register.at(field);
          uint16_t used_reg = insn->dest();

          old_to_new_reg.emplace(used_reg, stored_reg);
          deletes.push_back(insn);
        }
      }
    } else {
      // TODO(emmasevastian): allocate extra registers.
      if (insn->dests_size() > 0) {
        uint16_t current_dest = insn->dest();
        if (used_regs.find(current_dest) != used_regs.end()) {
          is_builder_removed = false;
          break;
        }
      }
    }
  }

  if (is_builder_removed) {
    auto transform = MethodTransform::get_method_transform(method);

    for (const auto& insn : deletes) {
      transform->remove_opcode(insn);
    }
  }

  return is_builder_removed;
}
