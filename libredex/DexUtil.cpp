/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexUtil.h"

#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <deque>
#include <unordered_set>

#include "Debug.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "EditableCfgAdapter.h"
#include "IRCode.h"
#include "Resolver.h"
#include "UnknownVirtuals.h"

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

DexType* get_integer_type() {
  return DexType::make_type("Ljava/lang/Integer;");
}

DexType* get_throwable_type() {
  return DexType::make_type("Ljava/lang/Throwable;");
}

ClassSerdes get_class_serdes(const DexClass* cls) {
  std::string name = cls->get_name()->str();
  name.pop_back();
  std::string flatbuf_name = name;
  std::replace(flatbuf_name.begin(), flatbuf_name.end(), '$', '_');

  std::string desername = name + "$Deserializer;";
  DexType* deser = DexType::get_type(desername.c_str());

  std::string flatbuf_desername = flatbuf_name + "Deserializer;";
  DexType* flatbuf_deser = DexType::get_type(flatbuf_desername.c_str());

  std::string sername = name + "$Serializer;";
  DexType* ser = DexType::get_type(sername);

  std::string flatbuf_sername = flatbuf_name + "Serializer;";
  DexType* flatbuf_ser = DexType::get_type(flatbuf_sername);

  return ClassSerdes(deser, flatbuf_deser, ser, flatbuf_ser);
}

std::string get_package_name(const DexType* type) {
  std::string name = std::string(type->get_name()->c_str());
  if (name.find("/") == std::string::npos) {
    return "";
  }
  unsigned long pos = name.find_last_of("/");
  return name.substr(0, pos);
}

std::string get_simple_name(const DexType* type) {
  std::string name = std::string(type->get_name()->c_str());
  if (name.find("/") == std::string::npos) {
    return name;
  }
  unsigned long pos_begin = name.find_last_of("/");
  unsigned long pos_end = name.find_last_of(";");
  return name.substr(pos_begin + 1, pos_end - pos_begin - 1);
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

bool is_trivial_clinit(const DexMethod* method) {
  always_assert(is_clinit(method));
  auto ii = InstructionIterable(method->get_code());
  return std::none_of(ii.begin(), ii.end(), [](const MethodItemEntry& mie) {
    return mie.insn->opcode() != OPCODE_RETURN_VOID;
  });
}

bool is_init(const DexMethodRef* method) {
  return strcmp(method->get_name()->c_str(), "<init>") == 0;
}

bool is_clinit(const DexMethodRef* method) {
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

DexType* get_array_component_type(const DexType* type) {
  if (!is_array(type)) return nullptr;
  auto name = type->get_name()->c_str();
  name++;
  return DexType::make_type(name);
}

DexType* make_array_type(const DexType* type) {
  always_assert(type != nullptr);
  return DexType::make_type(
      DexString::make_string("[" + type->get_name()->str()));
}

DexType* make_array_type(const DexType* type, uint32_t level) {
  always_assert(type != nullptr);
  if (level == 0) {
    return const_cast<DexType*>(type);
  }
  const auto elem_name = type->str();
  const uint32_t size = elem_name.size() + level;
  std::string name;
  name.reserve(size+1);
  name.append(level, '[');
  name.append(elem_name.begin(), elem_name.end());
  return DexType::make_type(name.c_str(), name.size());
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
  auto meth =
      DexMethod::make_method(DexType::make_type("Ljava/lang/RuntimeException;"),
                             DexString::make_string("<init>"), proto);
  auto invk = new IRInstruction(OPCODE_INVOKE_DIRECT);
  invk->set_method(meth);
  invk->set_srcs_size(2);
  invk->set_src(0, 0);
  invk->set_src(1, 1);
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

Scope build_class_scope(const DexStoresVector& stores) {
  return build_class_scope(DexStoreClassesIterator(stores));
}

void post_dexen_changes(const Scope& v, DexStoresVector& stores) {
  DexStoreClassesIterator iter(stores);
  post_dexen_changes(v, iter);
}

void load_root_dexen(DexStore& store,
                     const std::string& dexen_dir_str,
                     bool balloon,
                     bool verbose,
                     bool support_dex_v37) {
  namespace fs = boost::filesystem;
  fs::path dexen_dir_path(dexen_dir_str);
  redex_assert(fs::is_directory(dexen_dir_path));

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
      TRACE(MAIN, 1, "Loading %s", dex.string().c_str());
    }
    // N.B. throaway stats for now
    DexClasses classes =
        load_classes_from_dex(dex.string().c_str(), balloon, support_dex_v37);
    store.add_classes(std::move(classes));
  }
}

void create_store(const std::string& store_name,
                  DexStoresVector& stores,
                  DexClasses classes) {
  // First, remove the classes from other stores.
  for (auto& store : stores) {
    store.remove_classes(classes);
  }

  // Create a new store and add it to the list of stores.
  DexStore store(store_name);
  store.set_generated();
  store.add_classes(std::move(classes));
  stores.emplace_back(std::move(store));
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
  lhs.num_unique_types += rhs.num_unique_types;
  lhs.num_unique_protos += rhs.num_unique_protos;
  lhs.num_unique_strings += rhs.num_unique_strings;
  lhs.num_unique_method_refs += rhs.num_unique_method_refs;
  lhs.num_unique_field_refs += rhs.num_unique_field_refs;
  lhs.types_total_size += rhs.types_total_size;
  lhs.protos_total_size += rhs.protos_total_size;
  lhs.strings_total_size += rhs.strings_total_size;
  lhs.method_refs_total_size += rhs.method_refs_total_size;
  lhs.field_refs_total_size += rhs.field_refs_total_size;
  lhs.num_dbg_items += rhs.num_dbg_items;
  lhs.dbg_total_size += rhs.dbg_total_size;

  lhs.string_id_count += rhs.string_id_count;
  lhs.string_id_bytes += rhs.string_id_bytes;
  lhs.type_id_count += rhs.type_id_count;
  lhs.type_id_bytes += rhs.type_id_bytes;
  lhs.proto_id_count += rhs.proto_id_count;
  lhs.proto_id_bytes += rhs.proto_id_bytes;
  lhs.field_id_count += rhs.field_id_count;
  lhs.field_id_bytes += rhs.field_id_bytes;
  lhs.method_id_count += rhs.method_id_count;
  lhs.method_id_bytes += rhs.method_id_bytes;
  lhs.class_def_count += rhs.class_def_count;
  lhs.class_def_bytes += rhs.class_def_bytes;
  lhs.call_site_id_count += rhs.call_site_id_count;
  lhs.call_site_id_bytes += rhs.call_site_id_bytes;
  lhs.method_handle_count += rhs.method_handle_count;
  lhs.method_handle_bytes += rhs.method_handle_bytes;
  lhs.map_list_count += rhs.map_list_count;
  lhs.map_list_bytes += rhs.map_list_bytes;
  lhs.type_list_count += rhs.type_list_count;
  lhs.type_list_bytes += rhs.type_list_bytes;
  lhs.annotation_set_ref_list_count += rhs.annotation_set_ref_list_count;
  lhs.annotation_set_ref_list_bytes += rhs.annotation_set_ref_list_bytes;
  lhs.annotation_set_count += rhs.annotation_set_count;
  lhs.annotation_set_bytes += rhs.annotation_set_bytes;
  lhs.class_data_count += rhs.class_data_count;
  lhs.class_data_bytes += rhs.class_data_bytes;
  lhs.code_count += rhs.code_count;
  lhs.code_bytes += rhs.code_bytes;
  lhs.string_data_count += rhs.string_data_count;
  lhs.string_data_bytes += rhs.string_data_bytes;
  lhs.debug_info_count += rhs.debug_info_count;
  lhs.debug_info_bytes += rhs.debug_info_bytes;
  lhs.annotation_count += rhs.annotation_count;
  lhs.annotation_bytes += rhs.annotation_bytes;
  lhs.encoded_array_count += rhs.encoded_array_count;
  lhs.encoded_array_bytes += rhs.encoded_array_bytes;
  lhs.annotations_directory_count += rhs.annotations_directory_count;
  lhs.annotations_directory_bytes += rhs.annotations_directory_bytes;

  return lhs;
}

void relocate_method(DexMethod* method, DexType* to_type) {
  auto from_cls = type_class(method->get_class());
  auto to_cls = type_class(to_type);
  from_cls->remove_method(method);
  DexMethodSpec spec;
  spec.cls = to_type;
  method->change(spec,
                 true /* rename on collision */,
                 true /* update deobfuscated name */);
  to_cls->add_method(method);
}

bool is_subclass(const DexType* parent, const DexType* child) {
  auto super = child;
  while (super != nullptr) {
    if (parent == super) return true;
    const auto cls = type_class(super);
    if (cls == nullptr) break;
    super = cls->get_super_class();
  }
  return false;
}

void change_visibility(DexMethod* method, DexType* scope) {
  auto code = method->get_code();
  always_assert(code != nullptr);

  editable_cfg_adapter::iterate(code, [scope](MethodItemEntry& mie) {
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
      if (current_method != nullptr && current_method->is_concrete() &&
          (scope == nullptr || current_method->get_class() != scope)) {
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
    return editable_cfg_adapter::LOOP_CONTINUE;
  });

  std::vector<DexType*> types;
  if (code->editable_cfg_built()) {
    code->cfg().gather_catch_types(types);
  } else {
    code->gather_catch_types(types);
  }
  for (auto type : types) {
    auto cls = type_class(type);
    if (cls != nullptr && !cls->is_external()) {
      set_public(cls);
    }
  }
}

// Check that visibility / accessibility changes to the current method
// won't need to change a referenced method into a virtual or static one.
bool gather_invoked_methods_that_prevent_relocation(
    const DexMethod* method,
    std::unordered_set<DexMethodRef*>* methods_preventing_relocation) {
  auto code = method->get_code();
  always_assert(code);

  bool can_relocate = true;
  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    auto opcode = insn->opcode();
    if (is_invoke(opcode)) {
      auto meth = resolve_method(insn->get_method(), opcode_to_search(insn));
      if (!meth && opcode == OPCODE_INVOKE_VIRTUAL &&
          unknown_virtuals::is_method_known_to_be_public(insn->get_method())) {
        continue;
      }
      if (meth) {
        always_assert(meth->is_def());
        if (meth->is_external() && !is_public(meth)) {
          meth = nullptr;
        } else if (opcode == OPCODE_INVOKE_DIRECT && !is_init(meth)) {
          meth = nullptr;
        }
      }
      if (!meth) {
        can_relocate = false;
        if (!methods_preventing_relocation) {
          break;
        }
        methods_preventing_relocation->emplace(insn->get_method());
      }
    }
  }

  return can_relocate;
}

bool no_invoke_super(const DexMethod* method) {
  auto code = method->get_code();
  always_assert(code);

  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->opcode() == OPCODE_INVOKE_SUPER) {
      return false;
    }
  }

  return true;
}

bool relocate_method_if_no_changes(DexMethod* method, DexType* to_type) {
  if (!gather_invoked_methods_that_prevent_relocation(method)) {
    return false;
  }

  set_public(method);
  relocate_method(method, to_type);
  change_visibility(method);

  return true;
}

bool references_external(DexMethodRef* mref) {
  if (mref->is_external()) {
    return true;
  }
  auto ref_cls = type_class(mref->get_class());
  if (ref_cls && ref_cls->is_external()) {
    return true;
  }
  return false;
}

static const char* pure_method_names[] = {
    "Ljava/lang/Boolean;.booleanValue:()Z",
    "Ljava/lang/Boolean;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/Boolean;.getBoolean:(Ljava/lang/String;)Z",
    "Ljava/lang/Boolean;.hashCode:()I",
    "Ljava/lang/Boolean;.toString:()Ljava/lang/String;",
    "Ljava/lang/Boolean;.toString:(Z)Ljava/lang/String;",
    "Ljava/lang/Boolean;.valueOf:(Z)Ljava/lang/Boolean;",
    "Ljava/lang/Boolean;.valueOf:(Ljava/lang/String;)Ljava/lang/Boolean;",
    "Ljava/lang/Byte;.byteValue:()B",
    "Ljava/lang/Byte;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/Byte;.toString:()Ljava/lang/String;",
    "Ljava/lang/Byte;.toString:(B)Ljava/lang/String;",
    "Ljava/lang/Byte;.valueOf:(B)Ljava/lang/Byte;",
    "Ljava/lang/Class;.getName:()Ljava/lang/String;",
    "Ljava/lang/Class;.getSimpleName:()Ljava/lang/String;",
    "Ljava/lang/Double;.compare:(DD)I",
    "Ljava/lang/Double;.doubleValue:()D",
    "Ljava/lang/Double;.doubleToLongBits:(D)J",
    "Ljava/lang/Double;.doubleToRawLongBits:(D)J",
    "Ljava/lang/Double;.longBitsToDouble:(J)D",
    "Ljava/lang/Double;.valueOf:(D)Ljava/lang/Double;",
    "Ljava/lang/Enum;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/Enum;.name:()Ljava/lang/String;",
    "Ljava/lang/Enum;.ordinal:()I",
    "Ljava/lang/Float;.floatValue:()F",
    "Ljava/lang/Float;.compare:(FF)I",
    "Ljava/lang/Float;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/Float;.intBitsToFloat:(I)F",
    "Ljava/lang/Float;.floatToIntBits:(F)I",
    "Ljava/lang/Float;.isInfinite:(F)Z",
    "Ljava/lang/Float;.isNaN:(F)Z",
    "Ljava/lang/Float;.valueOf:(F)Ljava/lang/Float;",
    "Ljava/lang/Float;.toString:(F)Ljava/lang/String;",
    "Ljava/lang/Integer;.byteValue:()B",
    "Ljava/lang/Integer;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/Integer;.hashCode:()I",
    "Ljava/lang/Integer;.highestOneBit:(I)I",
    "Ljava/lang/Integer;.intValue:()I",
    "Ljava/lang/Integer;.longValue:()J",
    "Ljava/lang/Integer;.shortValue:()S",
    "Ljava/lang/Integer;.toBinaryString:(I)Ljava/lang/String;",
    "Ljava/lang/Integer;.toHexString:(I)Ljava/lang/String;",
    "Ljava/lang/Integer;.toString:(I)Ljava/lang/String;",
    "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;",
    "Ljava/lang/Long;.bitCount:(J)I",
    "Ljava/lang/Long;.compareTo:(Ljava/lang/Long;)I",
    "Ljava/lang/Long;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/Long;.hashCode:()I",
    "Ljava/lang/Long;.intValue:()I",
    "Ljava/lang/Long;.longValue:()J",
    "Ljava/lang/Long;.signum:(J)I",
    "Ljava/lang/Long;.toBinaryString:(J)Ljava/lang/String;",
    "Ljava/lang/Long;.toHexString:(J)Ljava/lang/String;",
    "Ljava/lang/Long;.toString:()Ljava/lang/String;",
    "Ljava/lang/Long;.toString:(J)Ljava/lang/String;",
    "Ljava/lang/Long;.valueOf:(J)Ljava/lang/Long;",
    "Ljava/lang/Math;.IEEEremainder:(DD)D",
    "Ljava/lang/Math;.abs:(J)J",
    "Ljava/lang/Math;.abs:(I)I",
    "Ljava/lang/Math;.abs:(F)F",
    "Ljava/lang/Math;.abs:(D)D",
    "Ljava/lang/Math;.acos:(D)D",
    "Ljava/lang/Math;.asin:(D)D",
    "Ljava/lang/Math;.atan:(D)D",
    "Ljava/lang/Math;.atan2:(DD)D",
    "Ljava/lang/Math;.cbrt:(D)D",
    "Ljava/lang/Math;.ceil:(D)D",
    "Ljava/lang/Math;.copySign:(FF)F",
    "Ljava/lang/Math;.copySign:(DD)D",
    "Ljava/lang/Math;.cos:(D)D",
    "Ljava/lang/Math;.cosh:(D)D",
    "Ljava/lang/Math;.exp:(D)D",
    "Ljava/lang/Math;.expm1:(D)D",
    "Ljava/lang/Math;.floor:(D)D",
    "Ljava/lang/Math;.floorDiv:(II)I",
    "Ljava/lang/Math;.floorDiv:(JJ)J",
    "Ljava/lang/Math;.floorMod:(JJ)J",
    "Ljava/lang/Math;.floorMod:(II)I",
    "Ljava/lang/Math;.getExponent:(D)I",
    "Ljava/lang/Math;.getExponent:(F)I",
    "Ljava/lang/Math;.hypot:(DD)D",
    "Ljava/lang/Math;.log:(D)D",
    "Ljava/lang/Math;.log10:(D)D",
    "Ljava/lang/Math;.log1p:(D)D",
    "Ljava/lang/Math;.max:(II)I",
    "Ljava/lang/Math;.max:(JJ)J",
    "Ljava/lang/Math;.max:(FF)F",
    "Ljava/lang/Math;.max:(DD)D",
    "Ljava/lang/Math;.min:(FF)F",
    "Ljava/lang/Math;.min:(DD)D",
    "Ljava/lang/Math;.min:(II)I",
    "Ljava/lang/Math;.min:(JJ)J",
    "Ljava/lang/Math;.nextAfter:(DD)D",
    "Ljava/lang/Math;.nextAfter:(FD)F",
    "Ljava/lang/Math;.nextDown:(D)D",
    "Ljava/lang/Math;.nextDown:(F)F",
    "Ljava/lang/Math;.nextUp:(F)F",
    "Ljava/lang/Math;.nextUp:(D)D",
    "Ljava/lang/Math;.pow:(DD)D",
    "Ljava/lang/Math;.random:()D",
    "Ljava/lang/Math;.rint:(D)D",
    "Ljava/lang/Math;.round:(D)J",
    "Ljava/lang/Math;.round:(F)I",
    "Ljava/lang/Math;.scalb:(FI)F",
    "Ljava/lang/Math;.scalb:(DI)D",
    "Ljava/lang/Math;.signum:(D)D",
    "Ljava/lang/Math;.signum:(F)F",
    "Ljava/lang/Math;.sin:(D)D",
    "Ljava/lang/Math;.sinh:(D)D",
    "Ljava/lang/Math;.sqrt:(D)D",
    "Ljava/lang/Math;.tan:(D)D",
    "Ljava/lang/Math;.tanh:(D)D",
    "Ljava/lang/Math;.toDegrees:(D)D",
    "Ljava/lang/Math;.toRadians:(D)D",
    "Ljava/lang/Math;.ulp:(D)D",
    "Ljava/lang/Math;.ulp:(F)F",
    "Ljava/lang/Object;.getClass:()Ljava/lang/Class;",
    "Ljava/lang/Short;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/Short;.shortValue:()S",
    "Ljava/lang/Short;.toString:(S)Ljava/lang/String;",
    "Ljava/lang/Short;.valueOf:(S)Ljava/lang/Short;",
    "Ljava/lang/String;.charAt:(I)C",
    "Ljava/lang/String;.concat:(Ljava/lang/String;)Ljava/lang/String;",
    "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z",
    "Ljava/lang/String;.equalsIgnoreCase:(Ljava/lang/String;)Z",
    "Ljava/lang/String;.hashCode:()I",
    "Ljava/lang/String;.indexOf:(I)I",
    "Ljava/lang/String;.isEmpty:()Z",
    "Ljava/lang/String;.lastIndexOf:(I)I",
    "Ljava/lang/String;.length:()I",
    "Ljava/lang/String;.startsWith:(Ljava/lang/String;)Z",
    "Ljava/lang/String;.substring:(I)Ljava/lang/String;",
    "Ljava/lang/String;.substring:(II)Ljava/lang/String;",
    "Ljava/lang/String;.trim:()Ljava/lang/String;",
    "Ljava/lang/String;.valueOf:(I)Ljava/lang/String;",
    "Ljava/lang/String;.valueOf:(J)Ljava/lang/String;",
    "Ljava/lang/String;.valueOf:(Z)Ljava/lang/String;",
    "Ljava/lang/System;.identityHashCode:(Ljava/lang/Object;)I",
    "Ljava/lang/Thread;.currentThread:()Ljava/lang/Thread;",
};

std::unordered_set<DexMethodRef*> get_pure_methods() {
  std::unordered_set<DexMethodRef*> pure_methods;
  for (auto const pure_method_name : pure_method_names) {
    auto method_ref = DexMethod::get_method(std::string(pure_method_name));
    if (method_ref == nullptr) {
      TRACE(CSE, 1, "[get_pure_methods]: Could not find pure method %s",
            pure_method_name);
      continue;
    }

    pure_methods.insert(method_ref);
  }
  return pure_methods;
}
