/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ObfuscateUtils.h"
#include "Trace.h"
#include <algorithm>

void DexFieldManager::commit_renamings_to_dex() {
  for (const auto& class_itr : this->elements) {
    for (const auto& type_itr : class_itr.second) {
      for (const auto& name_wrap : type_itr.second) {
        const FieldNameWrapper* wrap = name_wrap.second;
        if (!wrap->name_has_changed()) continue;
        DexField* field = wrap->get();
        always_assert_log(should_rename_elem(field),
          "Trying to rename (%s) %s:%s to %s, but we shouldn't\n",
          SHOW(field->get_type()), SHOW(field->get_class()), SHOW(field),
          wrap->get_name());
        TRACE(OBFUSCATE, 2,
          "\tRenaming the field 0x%x (%s) %s:%s to %s static value: 0x%x\n",
          field, SHOW(field->get_type()), SHOW(field->get_class()),
          SHOW(field->get_name()), wrap->get_name(), field->get_static_value());

        DexFieldRef ref;
        ref.name = DexString::make_string(wrap->get_name());
        field->change(ref);
      }
    }
  }
}

DexField* DexFieldManager::def_of_ref(DexField* ref) {
  DexType* cls = ref->get_class();
  while (type_class(cls)) {
    if (contains_elem(cls, sig_getter_fn(ref), ref->get_name())) {
      FieldNameWrapper* wrap(elements[cls][ref->get_type()][ref->get_name()]);
      if (wrap->name_has_changed())
        return wrap->get();
    }
    cls = type_class(cls)->get_super_class();
  }
  return nullptr;
}

void DexFieldManager::print_elements() {
  TRACE(OBFUSCATE, 4, "Field Ptr: (type) class old name -> new name\n");
  for (const auto& class_itr : elements) {
    for (const auto& type_itr : class_itr.second) {
      for (const auto& name_wrap : type_itr.second) {
        DexField* field = name_wrap.second->get();
        TRACE(OBFUSCATE, 4, " 0x%x: (%s) %s %s -> %s\n",
            field,
            SHOW(type_itr.first),
            SHOW(class_itr.first),
            SHOW(name_wrap.first),
            name_wrap.second->get_name());
      }
    }
  }
}

void DexMethodManager::commit_renamings_to_dex() {

}

DexMethod* DexMethodManager::def_of_ref(DexMethod* ref) {
  return nullptr;
}

void DexMethodManager::print_elements() {

}

void ClassVisitor::visit(DexClass* cls) {
  if (cls->is_external()) return;
  TRACE(OBFUSCATE, 3, "Visiting %s\n", cls->c_str());
  for (auto& field : cls->get_ifields())
    if ((should_visit_private() && is_private(field)) ||
        (should_visit_public() && !is_private(field)))
      member_visitor->visit_field(field);
  for (auto& field : cls->get_sfields())
    if ((should_visit_private() && is_private(field)) ||
        (should_visit_public() && !is_private(field)))
      member_visitor->visit_field(field);
  for (auto& method : cls->get_dmethods())
    if ((should_visit_private() && is_private(method)) ||
        (should_visit_public() && !is_private(method)))
      member_visitor->visit_method(method);
  for (auto& method : cls->get_vmethods())
    if ((should_visit_private() && is_private(method)) ||
        (should_visit_public() && !is_private(method)))
      member_visitor->visit_method(method);
}

// Walks the class hierarchy starting at this class and including
// superclasses (including external ones) and/or subclasses based on
// the specified HierarchyDirection.
void walk_hierarchy(
    DexClass* cls,
    ClassVisitor* visitor,
    HierarchyDirection h_dir) {
  if (!cls) return;
  visitor->visit(cls);

  // TODO: revisit for methods to be careful around Object
  if (h_dir & HierarchyDirection::VisitSuperClasses) {
    auto clazz = cls;
    while (clazz) {
      visitor->visit(clazz);
      if (clazz->get_super_class() == nullptr) break;
      clazz = type_class(clazz->get_super_class());
    }
  }

  if (h_dir & HierarchyDirection::VisitSubClasses) {
    for (auto subcls_type : get_children(cls->get_type())) {
      walk_hierarchy(type_class(subcls_type), visitor,
          HierarchyDirection::VisitSubClasses);
    }
  }
}

void rewrite_if_field_instr(FieldObfuscationState& f_ob_state,
    DexInstruction* instr) {
  // Only want field operations
  if (!instr->has_fields()) return;
  DexOpcodeField* field_instr = static_cast<DexOpcodeField*>(instr);

  DexField* field_ref = field_instr->field();
  if (field_ref->is_def()) return;
  TRACE(OBFUSCATE, 3, "Found a ref opcode\n");

  // Here we could use resolve_field to lookup the def, but this is
  // expensive, so we do resolution through ob_state which caches
  // and combines the lookup with the check for if we're changing the
  // field.
  DexField* field_def = f_ob_state.get_def_if_renamed(field_ref);
  if (field_def != nullptr) {
    TRACE(OBFUSCATE, 4, "Found a ref to fixup %s", SHOW(field_ref));
    field_instr->rewrite_field(field_def);
  }
}
