/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClassAssemblingUtils.h"

#include <boost/algorithm/string.hpp>

#include "Creators.h"
#include "DexStoreUtil.h"
#include "DexUtil.h"
#include "Model.h"
#include "Show.h"

using namespace class_merging;

namespace {

void patch_iget_for_int_like_types(DexMethod* meth,
                                   const IRList::iterator& it,
                                   IRInstruction* convert) {
  auto insn = it->insn;
  auto move_result_it = std::next(it);
  auto src_dest = move_result_it->insn->dest();
  convert->set_src(0, src_dest)->set_dest(src_dest);
  meth->get_code()->insert_after(move_result_it, convert);
  insn->set_opcode(OPCODE_IGET);
}

/**
 * Change the super class of a given class. The assumption is the new super
 * class has only one ctor and it shares the same signature with the old super
 * ctor.
 */
void change_super_class(DexClass* cls, DexType* super_type) {
  always_assert(cls);
  DexClass* super_cls = type_class(super_type);
  DexClass* old_super_cls = type_class(cls->get_super_class());
  always_assert(super_cls);
  always_assert(old_super_cls);
  auto super_ctors = super_cls->get_ctors();
  auto old_super_ctors = old_super_cls->get_ctors();
  // Assume that both the old and the new super only have one ctor
  always_assert(super_ctors.size() == 1);
  always_assert(old_super_ctors.size() == 1);

  // Fix calls to super_ctor in its ctors.
  // NOTE: we are not parallelizing this since the ctor is very short.
  size_t num_insn_fixed = 0;
  for (auto ctor : cls->get_ctors()) {
    TRACE(CLMG, 5, "Fixing ctor: %s", SHOW(ctor));
    auto code = ctor->get_code();
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (!opcode::is_invoke_direct(insn->opcode()) || !insn->has_method()) {
        continue;
      }
      // Replace "invoke_direct v0, old_super_type;.<init>:()V" with
      // "invoke_direct v0, super_type;.<init>:()V"
      if (insn->get_method() == old_super_ctors[0]) {
        TRACE(CLMG, 9, "  - Replacing call: %s with", SHOW(insn));
        insn->set_method(super_ctors[0]);
        TRACE(CLMG, 9, " %s", SHOW(insn));
        num_insn_fixed++;
      }
    }
  }
  TRACE(CLMG, 5, "Fixed %ld instructions", num_insn_fixed);

  cls->set_super_class(super_type);
  TRACE(CLMG, 5, "Added super class %s to %s", SHOW(super_type), SHOW(cls));
}

std::string get_merger_package_name(const DexType* type) {
  auto pkg_name = type::get_package_name(type);
  // Avoid an Android OS like package name, which might confuse the custom class
  // loader.
  if (boost::starts_with(pkg_name, "Landroid") ||
      boost::starts_with(pkg_name, "Ldalvik")) {
    return "Lcom/facebook/redex/";
  }
  return pkg_name;
}

DexType* create_empty_base_type(const ModelSpec& spec,
                                const DexType* interface_root,
                                const Scope& scope,
                                const DexStoresVector& stores) {
  auto cls = type_class(interface_root);
  if (cls == nullptr) {
    return nullptr;
  }
  if (!is_interface(cls)) {
    TRACE(CLMG, 1, "root %s is not an interface!", SHOW(interface_root));
    return nullptr;
  }

  ClassHierarchy ch = build_type_hierarchy(scope);
  InterfaceMap intf_map = build_interface_map(ch);
  std::unordered_map<const DexType*, TypeSet> implements;
  for (const auto& pair : intf_map) {
    for (const auto& type : pair.second) {
      implements[type].emplace(pair.first);
    }
  }

  // Create an empty base and add to the scope. Put the base class in the same
  // package as the root interface.
  auto base_type = DexType::make_type(
      DexString::make_string("L" + spec.class_name_prefix + "EmptyBase;"));
  auto base_class = create_class(base_type,
                                 type::java_lang_Object(),
                                 get_merger_package_name(interface_root),
                                 std::vector<DexField*>(),
                                 TypeSet(),
                                 true);

  TRACE(CLMG, 3, "Created an empty base class %s for interface %s.", SHOW(cls),
        SHOW(interface_root));

  // Set it as the super class of implementors.
  size_t num = 0;
  XStoreRefs xstores(stores);

  for (auto impl_type : get_all_implementors(intf_map, interface_root)) {
    if (type_class(impl_type)->is_external()) {
      TRACE(CLMG, 3, "Skip external implementer %s", SHOW(impl_type));
      continue;
    }
    auto& ifcs = implements.at(impl_type);
    // Add an empty base class to qualified implementors
    auto impl_cls = type_class(impl_type);
    if (ifcs.size() == 1 && impl_cls &&
        impl_cls->get_super_class() == type::java_lang_Object() &&
        !is_in_non_root_store(impl_type, stores, xstores,
                              spec.include_primary_dex)) {
      change_super_class(impl_cls, base_type);
      num++;
    }
  }

  return num > 0 ? base_type : nullptr;
}

} // namespace

namespace class_merging {

DexClass* create_class(const DexType* type,
                       const DexType* super_type,
                       const std::string& pkg_name,
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
  // Keeping class-merging generated classes from being renamed.
  cls->rstate.set_keepnames();

  if (!with_default_ctor) {
    return cls;
  }
  // Create ctor.
  auto super_ctors = type_class(super_type)->get_ctors();
  for (auto super_ctor : super_ctors) {
    auto mc = new MethodCreator(t,
                                DexString::make_string("<init>"),
                                super_ctor->get_proto(),
                                ACC_PUBLIC | ACC_CONSTRUCTOR);
    // Call to super.<init>
    std::vector<Location> args;
    size_t args_size = super_ctor->get_proto()->get_args()->size();
    for (size_t arg_loc = 0; arg_loc < args_size + 1; ++arg_loc) {
      args.push_back(mc->get_local(arg_loc));
    }
    auto mb = mc->get_main_block();
    mb->invoke(OPCODE_INVOKE_DIRECT, super_ctor, args);
    mb->ret_void();
    auto ctor = mc->create();
    TRACE(CLMG, 4, " default ctor created %s", SHOW(ctor));
    cls->add_method(ctor);
  }
  return cls;
}

std::vector<DexField*> create_merger_fields(
    const DexType* owner, const std::vector<DexField*>& mergeable_fields) {
  std::vector<DexField*> res;
  size_t cnt = 0;
  for (const auto f : mergeable_fields) {
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

    name = name + std::to_string(cnt);
    auto field = DexField::make_field(owner, DexString::make_string(name), type)
                     ->make_concrete(ACC_PUBLIC);
    res.push_back(field);
    cnt++;
  }

  TRACE(CLMG, 8, "  created merger fields %d ", res.size());
  return res;
}

void cook_merger_fields_lookup(
    const std::vector<DexField*>& new_fields,
    const FieldsMap& fields_map,
    std::unordered_map<DexField*, DexField*>& merger_fields_lookup) {
  for (const auto& fmap : fields_map) {
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

void patch_iput(const IRList::iterator& it) {
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

void patch_iget(DexMethod* meth,
                const IRList::iterator& it,
                DexType* original_field_type) {
  auto insn = it->insn;
  const auto op = insn->opcode();
  always_assert(opcode::is_an_iget(op));
  switch (op) {
  case OPCODE_IGET_OBJECT: {
    auto dest = std::next(it)->insn->dest();
    auto cast = ModelMethodMerger::make_check_cast(original_field_type, dest);
    meth->get_code()->insert_after(insn, cast);
    break;
  }
  case OPCODE_IGET_BYTE: {
    always_assert(original_field_type == type::_byte());
    auto int_to_byte = new IRInstruction(OPCODE_INT_TO_BYTE);
    patch_iget_for_int_like_types(meth, it, int_to_byte);
    break;
  }
  case OPCODE_IGET_CHAR: {
    always_assert(original_field_type == type::_char());
    auto int_to_char = new IRInstruction(OPCODE_INT_TO_CHAR);
    patch_iget_for_int_like_types(meth, it, int_to_char);
    break;
  }
  case OPCODE_IGET_SHORT: {
    always_assert(original_field_type == type::_short());
    auto int_to_short = new IRInstruction(OPCODE_INT_TO_SHORT);
    patch_iget_for_int_like_types(meth, it, int_to_short);
    break;
  }
  default:
    break;
  }
};

void add_class(DexClass* new_cls, Scope& scope, DexStoresVector& stores) {
  always_assert(new_cls != nullptr);

  scope.push_back(new_cls);
  TRACE(CLMG,
        4,
        " ClassMerging Adding class %s to scope %d ",
        SHOW(new_cls),
        scope.size());

  // TODO(emmasevastian): Handle this case in a better way.
  if (!stores.empty()) {
    DexClassesVector& root_store = stores.front().get_dexen();
    // Add the class to the last dex.
    root_store.back().emplace_back(new_cls);
  }
}

/**
 * In some limited cases we can do class merging on an interface when
 * implementors of the interface only implement that interface and have no
 * parent class other than java.lang.Object. We create a base class for those
 * implementors and use the new base class as root, and proceed with type
 * erasure as usual.
 */
void handle_interface_as_root(ModelSpec& spec,
                              Scope& scope,
                              DexStoresVector& stores) {
  TypeSet interface_roots;
  for (const auto root : spec.roots) {
    if (is_interface(type_class(root))) {
      interface_roots.insert(root);
    }
  }

  for (const auto interface_root : interface_roots) {
    auto empty_base =
        create_empty_base_type(spec, interface_root, scope, stores);
    if (empty_base != nullptr) {
      TRACE(CLMG, 3, "Changing the root from %s to %s.", SHOW(interface_root),
            SHOW(empty_base));
      spec.roots.insert(empty_base);
      add_class(type_class(empty_base), scope, stores);
    }
    // Remove interface roots regardless of whether an empty base was added.
    spec.roots.erase(interface_root);
  }
}

} // namespace class_merging
