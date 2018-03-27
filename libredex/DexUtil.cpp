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
#include <boost/regex.hpp>
#include <deque>
#include <unordered_set>

#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "IRCode.h"
#include "Resolver.h"

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

bool is_primitive(const DexType* type) {
  auto* const name = type->get_name()->c_str();
  switch (name[0]) {
    case 'Z':
    case 'B':
    case 'S':
    case 'C':
    case 'I':
    case 'J':
    case 'F':
    case 'D':
    case 'V':
      return true;
    case 'L':
    case '[':
      return false;
  }
  not_reached();
}

bool is_wide_type(const DexType* type) {
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

char type_shorty(const DexType* type) {
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

bool check_cast(const DexType* type, const DexType* base_type) {
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

bool is_object(const DexType* type) {
  char sig = type->get_name()->c_str()[0];
  return (sig == 'L') || (sig == '[');
}

bool is_integer(const DexType* type) {
  char sig = type->get_name()->c_str()[0];
  switch (sig) {
  case 'Z':
  case 'B':
  case 'S':
  case 'C':
  case 'I': {
    return true;
  }
  default: { return false; }
  }
}

bool is_boolean(const DexType* type) {
  return type->get_name()->c_str()[0] == 'Z';
}

bool is_long(const DexType* type) {
  return type->get_name()->c_str()[0] == 'J';
}

bool is_float(const DexType* type) {
  return type->get_name()->c_str()[0] == 'F';
}

bool is_double(const DexType* type) {
  return type->get_name()->c_str()[0] == 'D';
}

bool is_void(const DexType* type) {
  return type->get_name()->c_str()[0] == 'V';
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

DexType* make_array_type(const DexType* type) {
  always_assert(type != nullptr);
  return DexType::make_type(DexString::make_string("[" + std::string(type->get_name()->c_str())));
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
  size_t src_idx{0};
  size_t param_count{0};
  for (const auto& mie :
       InstructionIterable(code.get_param_instructions())) {
    auto load_param = mie.insn;
    ++param_count;
    if (src_idx >= insn->srcs_size()) {
      continue;
    }
    if (load_param->dest() != insn->src(src_idx++)) {
      return false;
    }
  }
  return insn->srcs_size() + ignore == param_count;
}

Scope build_class_scope(DexStoresVector& stores) {
  return build_class_scope(DexStoreClassesIterator(stores));
}

void post_dexen_changes(const Scope& v, DexStoresVector& stores) {
  DexStoreClassesIterator iter(stores);
  post_dexen_changes(v, iter);
}

void load_root_dexen(
  DexStore& store,
  const std::string& dexen_dir_str,
  bool balloon,
  bool verbose) {
  namespace fs = boost::filesystem;
  fs::path dexen_dir_path(dexen_dir_str);
  assert(fs::is_directory(dexen_dir_path));

  // Discover dex files
  auto end = fs::directory_iterator();
  std::vector<fs::path> dexen;
  for (fs::directory_iterator it(dexen_dir_path) ; it != end ; ++it) {
    auto file = it->path();
    if (fs::is_regular_file(file) &&
        !file.extension().compare(std::string(".dex"))) {
      dexen.emplace_back(file);
    }
  }

  /*
   * Comparator for dexen filename. 'classes.dex' should sort first,
   * followed by [^\d]*[\d]+.dex ordered by N numerically.
   */
  auto dex_comparator = [](const fs::path& a, const fs::path& b){
    boost::regex s_dex_regex("[^0-9]*([0-9]+)\\.dex");

    auto as = a.filename().string();
    auto bs = b.filename().string();
    boost::smatch amatch;
    boost::smatch bmatch;
    bool amatched = boost::regex_match(as, amatch, s_dex_regex);
    bool bmatched = boost::regex_match(bs, bmatch, s_dex_regex);

    if (!amatched && bmatched) {
      return true;
    } else if (amatched && !bmatched) {
      return false;
    } else if (!amatched && !bmatched) {
      // Compare strings, probably the same
      return strcmp(as.c_str(), bs.c_str()) > 0;
    } else {
      // Compare captures as integers
      auto anum = std::stoi(amatch[1]);
      auto bnum = std::stoi(bmatch[1]);
      return bnum > anum ;
    }
  };

  // Sort all discovered dex files
  std::sort(dexen.begin(), dexen.end(), dex_comparator);
  // Load all discovered dex files
  for (const auto& dex : dexen) {
    if (verbose) {
      TRACE(MAIN, 1, "Loading %s\n", dex.string().c_str());
    }
    // N.B. throaway stats for now
    DexClasses classes = load_classes_from_dex(dex.string().c_str(), balloon);
    store.add_classes(std::move(classes));
  }
}

/*
 * This exists because in the absence of a register allocator, we need each
 * transformation to keep the ins registers at the end of the frame. Once the
 * register allocator is switched on this function should no longer have many
 * use cases.
 */
size_t sum_param_sizes(const IRCode* code) {
  size_t size {0};
  auto param_ops = code->get_param_instructions();
  for (auto& mie : InstructionIterable(&param_ops)) {
    size += mie.insn->dest_is_wide() ? 2 : 1;
  }
  return size;
}

dex_stats_t&
  operator+=(dex_stats_t& lhs, const dex_stats_t& rhs) {
  lhs.num_types += rhs.num_types;
  lhs.num_classes += rhs.num_classes;
  lhs.num_methods += rhs.num_methods;
  lhs.num_method_refs += rhs.num_method_refs;
  lhs.num_fields += rhs.num_fields;
  lhs.num_field_refs += rhs.num_field_refs;
  lhs.num_strings += rhs.num_strings;
  lhs.num_protos += rhs.num_protos;
  lhs.num_static_values += rhs.num_static_values;
  lhs.num_annotations += rhs.num_annotations;
  lhs.num_type_lists += rhs.num_type_lists;
  lhs.num_bytes += rhs.num_bytes;
  lhs.num_instructions += rhs.num_instructions;
  return lhs;
}

void relocate_method(DexMethod* method, DexType* to_type) {
  auto from_cls = type_class(method->get_class());
  auto to_cls = type_class(to_type);
  from_cls->remove_method(method);
  DexMethodSpec spec;
  spec.cls = to_type;
  method->change(spec, true);
  to_cls->add_method(method);
}

void change_visibility(DexMethod* method) {
  auto code = method->get_code();
  always_assert(code != nullptr);

  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;

    if (insn->has_field()) {
      auto cls = type_class(insn->get_field()->get_class());
      if (cls != nullptr && !cls->is_external()) {
        set_public(cls);
      }
      auto field =
          resolve_field(insn->get_field(), is_sfield_op(insn->opcode())
              ? FieldSearch::Static : FieldSearch::Instance);
      if (field != nullptr && field->is_concrete()) {
        set_public(field);
        set_public(type_class(field->get_class()));
        // FIXME no point in rewriting opcodes in the method
        insn->set_field(field);
      }
    } else if (insn->has_method()) {
      auto cls = type_class(insn->get_method()->get_class());
      if (cls != nullptr && !cls->is_external()) {
        set_public(cls);
      }
      auto current_method = resolve_method(
          insn->get_method(), opcode_to_search(insn));
      if (current_method != nullptr && current_method->is_concrete()) {
        set_public(current_method);
        set_public(type_class(current_method->get_class()));
        // FIXME no point in rewriting opcodes in the method
        insn->set_method(current_method);
      }
    } else if (insn->has_type()) {
      auto type = insn->get_type();
      auto cls = type_class(type);
      if (cls != nullptr && !cls->is_external()) {
        set_public(cls);
      }
    }
  }

  std::vector<DexType*> types;
  method->get_code()->gather_catch_types(types);
  for (auto type : types) {
    auto cls = type_class(type);
    if (cls != nullptr && !cls->is_external()) {
      set_public(cls);
    }
  }
}

bool relocate_method_if_no_changes(DexMethod* method, DexType* to_type) {
  // Check that visibility / accessibility changes to the current method
  // won't need to change a referenced method into a virtual or static one.
  // If it does, we simply bail out.
  auto code = method->get_code();
  always_assert(code);

  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->opcode() == OPCODE_INVOKE_DIRECT) {
      auto meth = resolve_method(insn->get_method(), MethodSearch::Direct);
      if (!meth) {
        return false;
      }

      always_assert(meth->is_def());
      if (!is_init(meth)) {
        return false;
      }
    }
  }

  set_public(method);
  relocate_method(method, to_type);
  change_visibility(method);

  return true;
}
