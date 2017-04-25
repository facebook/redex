/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexUtil.h"

#include <boost/filesystem.hpp>
#include <unordered_set>

#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "Transform.h"

DexType* get_object_type() {
  return DexType::make_type("Ljava/lang/Object;");
}

DexType* get_void_type() {
   return DexType::make_type("V");
}

DexType* get_byte_type() {
  return DexType::make_type("B");
}

DexType* get_char_type() {
  return DexType::make_type("C");
}

DexType* get_short_type() {
  return DexType::make_type("S");
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

DexType* get_float_type() {
  return DexType::make_type("F");
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

bool is_wide_type(DexType* type) {
  auto const name = type->get_name()->c_str();
  switch (name[0]) {
  case 'J':
  case 'D':
    return true;
  default:
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

namespace {

// Find all the interfaces that extend 'intf'
bool gather_intf_extenders(const DexType* extender,
                           const DexType* intf,
                           std::unordered_set<const DexType*>& intf_extenders) {
  bool extends = false;
  const DexClass* extender_cls = type_class(extender);
  if (!extender_cls) return extends;
  if (is_interface(extender_cls)) {
    for (const auto& extends_intf :
         extender_cls->get_interfaces()->get_type_list()) {
      if (extends_intf == intf ||
          gather_intf_extenders(extends_intf, intf, intf_extenders)) {
        intf_extenders.insert(extender);
        extends = true;
      }
    }
  }
  return extends;
}

void gather_intf_extenders(const Scope& scope,
                           const DexType* intf,
                           std::unordered_set<const DexType*>& intf_extenders) {
  for (const auto& cls : scope) {
    gather_intf_extenders(cls->get_type(), intf, intf_extenders);
  }
}

}

void get_all_implementors(const Scope& scope,
                          const DexType* intf,
                          std::unordered_set<const DexType*>& impls) {
  std::unordered_set<const DexType*> intf_extenders;
  gather_intf_extenders(scope, intf, intf_extenders);

  std::unordered_set<const DexType*> intfs;
  intfs.insert(intf);
  intfs.insert(intf_extenders.begin(), intf_extenders.end());

  for (auto cls : scope) {
    auto cur = cls;
    bool found = false;
    while (!found && cur != nullptr) {
      for (auto impl : cur->get_interfaces()->get_type_list()) {
        if (intfs.count(impl) > 0) {
          impls.insert(cls->get_type());
          found = true;
          break;
        }
      }
      cur = type_class(cur->get_super_class());
    }
  }
}

void get_all_children_and_implementors(
    const Scope& scope,
    const DexClass* base_class,
    std::unordered_set<const DexType*>* result) {
  if (is_interface(base_class)) {
    std::unordered_set<const DexType*> impls;
    get_all_implementors(scope, base_class->get_type(), *result);
  } else {
    TypeVector children;
    get_all_children(base_class->get_type(), children);
    for (const auto& child : children) {
      result->emplace(child);
    }
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

const DexType* get_array_type_or_self(const DexType* type) {
  if (is_array(type)) {
    return get_array_type(type);
  }
  return type;
}

DexType* get_array_type(const DexType* type) {
  if (!is_array(type)) return nullptr;
  auto name = type->get_name()->c_str();
  while (*name == '[') {
    name++;
  }
  return DexType::make_type(name);
}

void create_runtime_exception_block(
    DexString* except_str, std::vector<IRInstruction*>& block) {
  // new-instance v0, Ljava/lang/RuntimeException; // type@3852
  // const-string v1, "Exception String e.g. Too many args" // string@7a6d
  // invoke-direct {v0, v1}, Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V
  // throw v0
  auto new_inst =
      (new IRInstruction(OPCODE_NEW_INSTANCE))
          ->set_type(DexType::make_type("Ljava/lang/RuntimeException;"));
  new_inst->set_dest(0);
  IRInstruction* const_inst =
      (new IRInstruction(OPCODE_CONST_STRING))->set_string(except_str);
  const_inst->set_dest(1);
  auto ret = DexType::make_type("V");
  auto arg = DexType::make_type("Ljava/lang/String;");
  auto args = DexTypeList::make_type_list({arg});
  auto proto = DexProto::make_proto(ret, args);
  auto meth = DexMethod::make_method(
    DexType::make_type("Ljava/lang/RuntimeException;"),
    DexString::make_string("<init>"), proto);
  auto invk = new IRInstruction(OPCODE_INVOKE_DIRECT);
  invk->set_method(meth);
  invk->set_arg_word_count(2);
  invk->set_src(0, 0); invk->set_src(1, 1);
  IRInstruction* throwinst = new IRInstruction(OPCODE_THROW);
  block.emplace_back(new_inst);
  block.emplace_back(const_inst);
  block.emplace_back(invk);
  block.emplace_back(throwinst);
}

bool passes_args_through(IRInstruction* insn,
                         const IRCode& code,
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

Scope build_class_scope(DexStoresVector& stores) {
  return build_class_scope(DexStoreClassesIterator(stores));
}

void post_dexen_changes(const Scope& v, DexStoresVector& stores) {
  DexStoreClassesIterator iter(stores);
  post_dexen_changes(v, iter);
}

void load_root_dexen(DexStore& store, const std::string& dexen_dir_str) {
  namespace fs = boost::filesystem;
  fs::path dexen_dir_path(dexen_dir_str);
  assert(fs::is_directory(dexen_dir_path));

  // Discover dex files
  auto end = fs::directory_iterator();
  std::vector<fs::path> dexen;
  for (fs::directory_iterator it(dexen_dir_path) ; it != end ; ++it) {
    auto file = it->path();
    if (fs::is_regular_file(file) && !file.extension().compare(".dex")) {
      dexen.emplace_back(file);
    }
  }

  /*
   * Comparator for dexen filename. 'classes.dex' should sort first,
   * followed by secondary-[N].dex ordered by N numerically.
   */
  auto dex_comparator = [](const fs::path& a, const fs::path& b){
    auto as = a.stem().string();
    auto bs = b.stem().string();
    bool adashed = as.rfind("-") != std::string::npos;
    bool bdashed = bs.rfind("-") != std::string::npos;
    if (!adashed && bdashed) {
      return true;
    } else if (adashed && !bdashed) {
      return false;
    } else if (!adashed && !bdashed) {
      return strcmp(as.c_str(), bs.c_str()) > 1;
    } else {
      auto anum = atoi(as.substr(as.rfind("-") + 1).c_str());
      auto bnum = atoi(bs.substr(bs.rfind("-") + 1).c_str());
      return bnum > anum ;
    }
  };

  // Sort all discovered dex files
  std::sort(dexen.begin(), dexen.end(), dex_comparator);
  // Load all discovered dex files
  for (const auto& dex : dexen) {
    std::cout << "Loading " << dex.string() << std::endl;
    DexClasses classes = load_classes_from_dex(dex.c_str(), /* balloon */ false);
    store.add_classes(std::move(classes));
  }
}
