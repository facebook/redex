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

void rename_method(DexMethod* method,
      const std::string& new_name) {
  // To be done. This is a dummy definition.
  always_assert(should_rename_method(method));
  DexMethodRef ref;
  std::string old_name = method->get_name()->c_str();
  ref.name = DexString::make_string(new_name);
  method->change(ref);
}

std::string NameGenerator::next_name() {
  std::string res = "";
  do {
    int ctr_cpy = ctr;
    res.clear();
    while(ctr_cpy > 0) {
      res += get_ident(ctr_cpy % kMaxIdentChar);
      ctr_cpy /= kMaxIdentChar;
    }
    ctr += 1;
    TRACE(OBFUSCATE, 4, "NameGenerator looking for a name, trying: %s\n",
        res.c_str());
  } while(ids_to_avoid.count(res) > 0 || used_ids->count(res) > 0);
  return res;
}

void DexFieldManager::commit_renamings_to_dex() {
  for (const auto& class_itr : elements) {
    for (const auto& type_itr : class_itr.second) {
      for (const auto& name_wrap : type_itr.second) {
        const FieldNameWrapper& wrap(name_wrap.second);
        if (!wrap.name_has_changed()) continue;
        DexField* field = wrap.get();
        always_assert_log(should_rename_field(field),
          "Trying to rename (%s) %s:%s to %s, but we shouldn't\n",
          SHOW(field->get_type()), SHOW(field->get_class()), SHOW(field),
          wrap.get_name());
        TRACE(OBFUSCATE, 2,
          "\tRenaming the field 0x%x (%s) %s:%s to %s static value: 0x%x\n",
          field, SHOW(field->get_type()), SHOW(field->get_class()),
          SHOW(field->get_name()), wrap.get_name(), field->get_static_value());

        DexFieldRef ref;
        ref.name = DexString::make_string(wrap.get_name());
        field->change(ref);
      }
    }
  }
}

DexField* DexFieldManager::def_of_ref(DexField* ref) {
  DexType* cls = ref->get_class();
  while (type_class(cls)) {
    if (contains_field(cls, ref->get_type(), ref->get_name())) {
      FieldNameWrapper& wrap(elements[cls][ref->get_type()][ref->get_name()]);
      if (wrap.name_has_changed())
        return wrap.get();
    }
    cls = type_class(cls)->get_super_class();
  }
  return nullptr;
}

void DexFieldManager::print_elements() {
  TRACE(OBFUSCATE, 4, "Field Ptr: class old name -> new name\n");
  for (const auto& class_itr : elements) {
    for (const auto& type_itr : class_itr.second) {
      for (const auto& name_wrap : type_itr.second) {
        DexField* field = name_wrap.second.get();
        TRACE(OBFUSCATE, 4, " 0x%x: %s%s -> %s\n",
            field,
            SHOW(field->get_class()),
            SHOW(field),
            name_wrap.second.get_name());
      }
    }
  }
}

void ClassVisitor::visit(DexClass* cls) {
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

DexField* ObfuscationState::get_def_if_renamed(DexField* field_ref) {
  auto itr = ref_def_cache.find(field_ref);
  if (itr != ref_def_cache.end())
    return itr->second;
  DexField* def = name_mapping.def_of_ref(field_ref);
  ref_def_cache[field_ref] = def;
  return def;
}

bool contains_renamable_field(const std::list<DexField*>& fields) {
  if (fields.size() == 0) return false;
  for (DexField* f : fields) {
    if (should_rename_field(f)) return true;
  }
  return false;
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
