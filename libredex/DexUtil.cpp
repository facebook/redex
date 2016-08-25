/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexUtil.h"

#include <unordered_set>

#include "Debug.h"
#include "DexClass.h"

DexType* get_object_type() {
  return DexType::make_type("Ljava/lang/Object;");
}

DexType* get_void_type() {
   return DexType::make_type("V");
}

DexType* get_int_type() {
  return DexType::make_type("I");
}

DexType* get_long_type() {
  return DexType::make_type("J");
}

DexType* get_boolean_type() {
  return DexType::make_type("Z");
}

DexType* get_double_type() {
  return DexType::make_type("D");
}

DexType* get_string_type() {
  return DexType::make_type("Ljava/lang/String;");
}

DexType* get_class_type() {
  return DexType::make_type("Ljava/lang/Class;");
}

DexType* get_enum_type() {
  return DexType::make_type("Ljava/lang/Enum;");
}

bool is_primitive(DexType* type) {
  auto const name = type->get_name()->c_str();
  switch (name[0]) {
  case 'Z':
  case 'B':
  case 'S':
  case 'C':
  case 'I':
  case 'J':
  case 'F':
  case 'D':
    return true;
  case 'L':
  case '[':
  case 'V':
    return false;
  }
  not_reached();
}

DataType type_to_datatype(const DexType* t) {
  auto const name = t->get_name()->c_str();
  switch (name[0]) {
  case 'V':
    return DataType::Void;
  case 'Z':
    return DataType::Boolean;
  case 'B':
    return DataType::Byte;
  case 'S':
    return DataType::Short;
  case 'C':
    return DataType::Char;
  case 'I':
    return DataType::Int;
  case 'J':
    return DataType::Long;
  case 'F':
    return DataType::Float;
  case 'D':
    return DataType::Double;
  case 'L':
    return DataType::Object;
  case '[':
    return DataType::Array;
  }
  not_reached();
}

char type_shorty(DexType* type) {
  auto const name = type->get_name()->c_str();
  switch (name[0]) {
  case '[':
    return 'L';
  case 'V':
  case 'Z':
  case 'B':
  case 'S':
  case 'C':
  case 'I':
  case 'J':
  case 'F':
  case 'D':
  case 'L':
    return name[0];
  }
  not_reached();
}

bool check_cast(DexType* type, DexType* base_type) {
  if (type == base_type) return true;
  const auto cls = type_class(type);
  if (cls == nullptr) return false;
  if (check_cast(cls->get_super_class(), base_type)) return true;
  auto intfs = cls->get_interfaces();
  for (auto intf : intfs->get_type_list()) {
    if (check_cast(intf, base_type)) return true;
  }
  return false;
}

bool has_hierarchy_in_scope(DexClass* cls) {
  DexType* super = nullptr;
  const DexClass* super_cls = cls;
  while (super_cls) {
    super = super_cls->get_super_class();
    super_cls = type_class_internal(super);
  }
  return super == get_object_type();
}

void get_all_children(const DexType* type, TypeVector& children) {
  const auto& direct = get_children(type);
  for (const auto& child : direct) {
    children.push_back(child);
    get_all_children(child, children);
  }
}

bool is_init(const DexMethod* method) {
  return strcmp(method->get_name()->c_str(), "<init>") == 0;
}

bool is_clinit(const DexMethod* method) {
  return strcmp(method->get_name()->c_str(), "<clinit>") == 0;
}

DexAccessFlags merge_visibility(uint32_t vis1, uint32_t vis2) {
  vis1 &= VISIBILITY_MASK;
  vis2 &= VISIBILITY_MASK;
  if ((vis1 & ACC_PUBLIC) || (vis2 & ACC_PUBLIC)) return ACC_PUBLIC;
  if (vis1 == 0 || vis2 == 0) return static_cast<DexAccessFlags>(0);
  if ((vis1 & ACC_PROTECTED) || (vis2 & ACC_PROTECTED)) return ACC_PROTECTED;
  return ACC_PRIVATE;
}

bool is_array(const DexType* type) {
  return type->get_name()->c_str()[0] == '[';
}

uint32_t get_array_level(const DexType* type) {
  auto name = type->get_name()->c_str();
  uint32_t level = 0;
  while (*name++ == '[' && ++level)
    ;
  return level;
}

DexType* get_array_type(const DexType* type) {
  if (!is_array(type)) return nullptr;
  auto name = type->get_name()->c_str();
  while (*name == '[') {
    name++;
  }
  return DexType::make_type(name);
}

bool passes_args_through(DexOpcodeMethod* insn,
                         const DexCode& code,
                         int ignore /* = 0 */
                         ) {
  auto regs = code.get_registers_size();
  auto ins = code.get_ins_size();
  auto wc = insn->arg_word_count();
  if (wc != (code.get_ins_size() - ignore)) return false;
  for (int i = 0; i < wc; i++) {
    if (insn->src(i) != (regs - ins + i)) {
      return false;
    }
  }
  return true;
}

Scope build_class_scope(const DexClassesVector& dexen) {
  Scope v;
  for (auto const& classes : dexen) {
    for (auto clazz : classes) {
      v.push_back(clazz);
    }
  }
  return v;
}

void post_dexen_changes(const Scope& v, DexClassesVector& dexen) {
  std::unordered_set<DexClass*> clookup(v.begin(), v.end());
  for (auto& classes : dexen) {
    classes.erase(
      std::remove_if(
        classes.begin(),
        classes.end(),
        [&](DexClass* cls) {
          return !clookup.count(cls);
        }),
      classes.end());
  }
  if (debug) {
    std::unordered_set<DexClass*> dlookup;
    for (auto const& classes : dexen) {
      for (auto const& cls : classes) {
        dlookup.insert(cls);
      }
    }
    for (auto const& cls : clookup) {
      assert_log(dlookup.count(cls), "Can't add classes in post_dexen_changes");
    }
  }
}
