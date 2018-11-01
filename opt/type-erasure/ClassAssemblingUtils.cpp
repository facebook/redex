/**
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

namespace {

void patch_iget_for_int_like_types(DexMethod* meth,
                                   IRList::iterator it,
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
    TRACE(TERA, 5, "Fixing ctor: %s\n", SHOW(ctor));
    auto code = ctor->get_code();
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (!is_invoke_direct(insn->opcode()) || !insn->has_method()) {
        continue;
      }
      // Replace "invoke_direct v0, old_super_type;.<init>:()V" with
      // "invoke_direct v0, super_type;.<init>:()V"
      if (insn->get_method() == old_super_ctors[0]) {
        TRACE(TERA, 9, "  - Replacing call: %s with", SHOW(insn));
        insn->set_method(super_ctors[0]);
        TRACE(TERA, 9, " %s\n", SHOW(insn));
        num_insn_fixed++;
      }
    }
  }
  TRACE(TERA, 5, "Fixed %ld instructions\n", num_insn_fixed);

  cls->set_super_class(super_type);
  TRACE(TERA, 5, "Added super class %s to %s\n", SHOW(super_type), SHOW(cls));
}

std::string get_merger_package_name(const DexType* type) {
  auto pkg_name = get_package_name(type);
  // Avoid an Android OS like package name, which might confuse the custom class
  // loader.
  if (boost::starts_with(pkg_name, "Landroid") ||
      boost::starts_with(pkg_name, "Ldalvik")) {
    return "Lcom/facebook/redex";
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
    TRACE(TERA, 1, "root %s is not an interface!\n", SHOW(interface_root));
    return nullptr;
  }

  // Build a temporary type system.
  TypeSystem type_system(scope);

  // Create an empty base and add to the scope. Put the base class in the same
  // package as the root interface.
  auto base_type = DexType::make_type(
      DexString::make_string("L" + spec.class_name_prefix + "EmptyBase;"));
  auto base_class = create_class(base_type,
                                 get_object_type(),
                                 get_merger_package_name(interface_root),
                                 std::vector<DexField*>(),
                                 TypeSet(),
                                 true);

  TRACE(TERA, 3, "Created an empty base class %s for interface %s.\n",
        SHOW(cls), SHOW(interface_root));

  // Set it as the super class of implementors.
  size_t num = 0;
  XStoreRefs xstores(stores);

  for (auto impl_type : type_system.get_implementors(interface_root)) {
    if (type_class(impl_type)->is_external()) {
      TRACE(TERA, 3, "Skip external implementer %s\n", SHOW(impl_type));
      continue;
    }
    auto& ifcs = type_system.get_implemented_interfaces(impl_type);
    // Add an empty base class to qualified implementors
    auto impl_cls = type_class(impl_type);
    if (ifcs.size() == 1 && impl_cls &&
        impl_cls->get_super_class() == get_object_type() &&
        !is_in_non_root_store(impl_type, stores, xstores,
                              spec.include_primary_dex)) {
      change_super_class(impl_cls, base_type);
      num++;
    }
  }

  return num > 0 ? base_type : nullptr;
}

} // namespace

DexClass* create_class(const DexType* type,
                       const DexType* super_type,
                       const std::string& pkg_name,
                       std::vector<DexField*> fields,
                       const TypeSet& interfaces,
                       bool with_default_ctor,
                       DexAccessFlags access) {
  DexType* t = const_cast<DexType*>(type);
  always_assert(!pkg_name.empty());
  auto name = std::string(type->get_name()->c_str());
  name = pkg_name + "/" + name.substr(1);
  t->assign_name_alias(DexString::make_string(name));
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
  }
  auto cls = creator.create();
  // Keeping type-erasure generated classes from being renamed.
  cls->rstate.set_keep_name();

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
    TRACE(TERA, 4, " default ctor created %s\n", SHOW(ctor));
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
    if (type == get_byte_type() || type == get_char_type() ||
        type == get_short_type() || type == get_int_type()) {
      type = get_int_type();
      name = "i";
    } else if (type == get_boolean_type()) {
      type = get_boolean_type();
      name = "z";
    } else if (type == get_long_type()) {
      type = get_long_type();
      name = "j";
    } else if (type == get_float_type()) {
      type = get_float_type();
      name = "f";
    } else if (type == get_double_type()) {
      type = get_double_type();
      name = "d";
    } else {
      static DexType* string_type = DexType::make_type("Ljava/lang/String;");
      if (type == string_type) {
        type = string_type;
        name = "s";
      } else {
        char t = type_shorty(type);
        always_assert(t == 'L' || t == '[');
        type = get_object_type();
        name = "l";
      }
    }

    name = name + std::to_string(cnt);
    auto field = static_cast<DexField*>(
        DexField::make_field(owner, DexString::make_string(name), type));
    field->make_concrete(ACC_PUBLIC);
    res.push_back(field);
    cnt++;
  }

  TRACE(TERA, 8, "  created merger fields %d \n", res.size());
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
    auto type_tag_field = static_cast<DexField*>(DexField::make_field(
        type, DexString::make_string(INTERNAL_TYPE_TAG_FIELD_NAME),
        get_int_type()));
    type_tag_field->make_concrete(ACC_PUBLIC | ACC_FINAL);
    fields.push_back(type_tag_field);
  }

  for (auto f : merger_fields) {
    fields.push_back(f);
  }
  // Put merger class in the same package as super_type.
  auto pkg_name = get_merger_package_name(super_type);
  auto cls = create_class(type, super_type, pkg_name, fields, interfaces,
                          with_default_ctor);
  TRACE(TERA, 3, "  created merger class w/ fields %s \n", SHOW(cls));
  return cls;
}

void patch_iput(IRList::iterator it) {
  auto insn = it->insn;
  const auto op = insn->opcode();
  always_assert(is_iput(op));
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
                IRList::iterator it,
                DexType* original_field_type) {
  auto insn = it->insn;
  const auto op = insn->opcode();
  always_assert(is_iget(op));
  switch (op) {
  case OPCODE_IGET_OBJECT: {
    auto dest = std::next(it)->insn->dest();
    auto cast = MethodMerger::make_check_cast(original_field_type, dest);
    meth->get_code()->insert_after(insn, cast);
    break;
  }
  case OPCODE_IGET_BYTE: {
    always_assert(original_field_type == get_byte_type());
    auto int_to_byte = new IRInstruction(OPCODE_INT_TO_BYTE);
    patch_iget_for_int_like_types(meth, it, int_to_byte);
    break;
  }
  case OPCODE_IGET_CHAR: {
    always_assert(original_field_type == get_char_type());
    auto int_to_char = new IRInstruction(OPCODE_INT_TO_CHAR);
    patch_iget_for_int_like_types(meth, it, int_to_char);
    break;
  }
  case OPCODE_IGET_SHORT: {
    always_assert(original_field_type == get_short_type());
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
  TRACE(TERA,
        4,
        " TERA Adding class %s to scope %d \n",
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
 * In some limited cases we can do type erasure on an interface when
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
      TRACE(TERA, 3, "Changing the root from %s to %s.\n", SHOW(interface_root),
            SHOW(empty_base));
      spec.roots.erase(interface_root);
      spec.roots.insert(empty_base);
      add_class(type_class(empty_base), scope, stores);
    }
  }
}
