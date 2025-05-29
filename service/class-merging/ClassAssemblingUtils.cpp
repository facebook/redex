/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClassAssemblingUtils.h"

#include <boost/algorithm/string.hpp>

#include "Creators.h"
#include "DexStore.h"
#include "DexUtil.h"
#include "Model.h"
#include "ReachableClasses.h"
#include "Show.h"
#include "Trace.h"

using namespace class_merging;

namespace {

void patch_iget_for_int_like_types(cfg::ControlFlowGraph& cfg,
                                   const cfg::InstructionIterator& it,
                                   IRInstruction* convert) {
  auto insn = it->insn;
  auto move_result_it = cfg.move_result_of(it);
  auto src_dest = move_result_it->insn->dest();
  convert->set_src(0, src_dest)->set_dest(src_dest);
  cfg.insert_after(move_result_it, convert);
  insn->set_opcode(OPCODE_IGET);
}

std::string_view get_merger_package_name(const DexType* root_type) {
  auto pkg_name = type::get_package_name(root_type);
  // Avoid an Android OS like package name, which might confuse the custom class
  // loader. See T34120391 for details.
  if (boost::starts_with(pkg_name, "Landroid/") ||
      boost::starts_with(pkg_name, "Ldalvik/") ||
      boost::starts_with(pkg_name, "Ljava/")) {
    auto cls = type_class(root_type);
    if (cls && cls->is_external()) {
      return "Lcom/facebook/redex/";
    }
  }
  return pkg_name;
}

} // namespace

namespace class_merging {

DexClass* create_class(const DexType* type,
                       const DexType* super_type,
                       const std::string_view pkg_name,
                       const std::vector<DexField*>& fields,
                       const TypeSet& interfaces,
                       bool with_default_ctor,
                       DexAccessFlags access) {
  DexType* t = const_cast<DexType*>(type);
  always_assert(!pkg_name.empty());
  auto name = std::string(type->get_name()->c_str());
  name = pkg_name + name.substr(1);
  t->set_name(DexString::make_string(name));
  // Create class.
  ClassCreator creator(t);
  creator.set_access(access);
  always_assert(super_type != nullptr);
  creator.set_super(const_cast<DexType*>(super_type));
  for (const auto& itf : interfaces) {
    creator.add_interface(const_cast<DexType*>(itf));
  }
  for (const auto& field : fields) {
    creator.add_field(field);
    field->set_deobfuscated_name(show_deobfuscated(field));
  }
  auto cls = creator.create();

  if (!with_default_ctor) {
    return cls;
  }
  // Create ctor.
  auto super_ctors = type_class(super_type)->get_ctors();
  for (auto super_ctor : super_ctors) {
    auto mc = MethodCreator(t,
                            DexString::make_string("<init>"),
                            super_ctor->get_proto(),
                            ACC_PUBLIC | ACC_CONSTRUCTOR);
    // Call to super.<init>
    std::vector<Location> args;
    size_t args_size = super_ctor->get_proto()->get_args()->size();
    for (size_t arg_loc = 0; arg_loc < args_size + 1; ++arg_loc) {
      args.push_back(mc.get_local(arg_loc));
    }
    auto mb = mc.get_main_block();
    mb->invoke(OPCODE_INVOKE_DIRECT, super_ctor, args);
    mb->ret_void();
    auto ctor = mc.create();
    ctor->get_code()->build_cfg();
    TRACE(CLMG, 4, " default ctor created %s", SHOW(ctor));
    cls->add_method(ctor);
  }
  return cls;
}

std::vector<DexField*> create_merger_fields(const DexType* owner,
                                            const FieldsMap& fields_map) {
  std::vector<DexField*> res;
  const std::vector<DexField*>& mergeable_fields =
      unordered_any(fields_map)->second;
  size_t cnt = 0;
  for (const DexField* f : mergeable_fields) {
    auto type = f->get_type();
    std::string name;
    if (type == type::_byte() || type == type::_char() ||
        type == type::_short() || type == type::_int()) {
      type = type::_int();
      name = "i";
    } else if (type == type::_boolean()) {
      type = type::_boolean();
      name = "z";
    } else if (type == type::_long()) {
      type = type::_long();
      name = "j";
    } else if (type == type::_float()) {
      type = type::_float();
      name = "f";
    } else if (type == type::_double()) {
      type = type::_double();
      name = "d";
    } else {
      static DexType* string_type = DexType::make_type("Ljava/lang/String;");
      if (type == string_type) {
        type = string_type;
        name = "s";
      } else {
        char t = type::type_shorty(type);
        always_assert(t == 'L' || t == '[');
        type = type::java_lang_Object();
        name = "l";
      }
    }

    name += std::to_string(cnt);
    auto field = DexField::make_field(owner, DexString::make_string(name), type)
                     ->make_concrete(ACC_PUBLIC);
    res.push_back(field);
    cnt++;
  }

  std::vector<bool> acc_final_map(mergeable_fields.size(), true);
  for (const auto& fmap : UnorderedIterable(fields_map)) {
    const auto& fields = fmap.second;
    for (size_t i = 0; i < fields.size(); i++) {
      redex_assert(acc_final_map.size() > i);
      acc_final_map[i] = acc_final_map.at(i) && is_final(fields[i]);
    }
  }

  always_assert(res.size() == mergeable_fields.size());
  for (size_t i = 0; i < res.size(); i++) {
    if (acc_final_map[i]) {
      DexField* f = res.at(i);
      always_assert(f != nullptr && f->is_def());
      set_final(f);
      TRACE(CLMG, 5, "marking merger field final %s", SHOW(f));
    }
  }

  TRACE(CLMG, 8, "  created merger fields %zu ", res.size());
  return res;
}

void cook_merger_fields_lookup(
    const std::vector<DexField*>& new_fields,
    const FieldsMap& fields_map,
    UnorderedMap<DexField*, DexField*>& merger_fields_lookup) {
  for (const auto& fmap : UnorderedIterable(fields_map)) {
    const auto& old_fields = fmap.second;
    always_assert(new_fields.size() == old_fields.size());
    for (size_t i = 0; i < new_fields.size(); i++) {
      if (old_fields.at(i) != nullptr) {
        merger_fields_lookup[old_fields.at(i)] = new_fields.at(i);
      }
    }
  }
}

DexClass* create_merger_class(const DexType* type,
                              const DexType* super_type,
                              const std::vector<DexField*>& merger_fields,
                              const TypeSet& interfaces,
                              bool add_type_tag_field,
                              bool with_default_ctor /* false */) {
  always_assert(type && super_type);
  std::vector<DexField*> fields;

  if (add_type_tag_field) {
    auto type_tag_field =
        DexField::make_field(
            type, DexString::make_string(INTERNAL_TYPE_TAG_FIELD_NAME),
            type::_int())
            ->make_concrete(ACC_PUBLIC | ACC_FINAL);
    type_tag_field->rstate.set_name_used();
    fields.push_back(type_tag_field);
  }

  for (auto f : merger_fields) {
    fields.push_back(f);
  }
  // Put merger class in the same package as super_type.
  auto pkg_name = get_merger_package_name(super_type);
  auto cls = create_class(type, super_type, pkg_name, fields, interfaces,
                          with_default_ctor);
  TRACE(CLMG, 3, "  created merger class w/ fields %s ", SHOW(cls));
  return cls;
}

void patch_iput(const cfg::InstructionIterator& it) {
  auto insn = it->insn;
  const auto op = insn->opcode();
  always_assert(opcode::is_an_iput(op));
  switch (op) {
  case OPCODE_IPUT_BYTE:
  case OPCODE_IPUT_CHAR:
  case OPCODE_IPUT_SHORT:
    insn->set_opcode(OPCODE_IPUT);
    break;
  default:
    break;
  }
};

void patch_iget(cfg::ControlFlowGraph& cfg,
                const cfg::InstructionIterator& it,
                DexType* original_field_type) {
  auto insn = it->insn;
  const auto op = insn->opcode();
  always_assert(opcode::is_an_iget(op));
  switch (op) {
  case OPCODE_IGET_OBJECT: {
    auto move_insn_it = cfg.move_result_of(it);
    auto dest = move_insn_it->insn->dest();
    auto cast = ModelMethodMerger::make_check_cast(original_field_type, dest);
    cfg.insert_after(move_insn_it, cast);
    break;
  }
  case OPCODE_IGET_BYTE: {
    always_assert(original_field_type == type::_byte());
    auto int_to_byte = new IRInstruction(OPCODE_INT_TO_BYTE);
    patch_iget_for_int_like_types(cfg, it, int_to_byte);
    break;
  }
  case OPCODE_IGET_CHAR: {
    always_assert(original_field_type == type::_char());
    auto int_to_char = new IRInstruction(OPCODE_INT_TO_CHAR);
    patch_iget_for_int_like_types(cfg, it, int_to_char);
    break;
  }
  case OPCODE_IGET_SHORT: {
    always_assert(original_field_type == type::_short());
    auto int_to_short = new IRInstruction(OPCODE_INT_TO_SHORT);
    patch_iget_for_int_like_types(cfg, it, int_to_short);
    break;
  }
  default:
    break;
  }
};

void add_class(DexClass* new_cls,
               Scope& scope,
               DexStoresVector& stores,
               boost::optional<size_t> dex_id) {
  always_assert(new_cls != nullptr);
  TRACE(CLMG, 4, " ClassMerging Adding class %s to dex(%s) scope[%zu]",
        SHOW(new_cls), dex_id ? std::to_string(*dex_id).c_str() : "last",
        scope.size());
  scope.push_back(new_cls);
  DexStore::add_class(new_cls, stores, dex_id);
}

} // namespace class_merging
