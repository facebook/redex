/*
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
#include "DexAsm.h"
#include "DexClass.h"
#include "DexLoader.h"
#include "EditableCfgAdapter.h"
#include "IRCode.h"
#include "Resolver.h"
#include "UnknownVirtuals.h"

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

DexMethod* get_or_create_clinit(DexClass* cls) {
  using namespace dex_asm;

  auto clinit_name = DexString::make_string("<clinit>");
  auto clinit_proto =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));

  DexMethod* clinit = static_cast<DexMethod*>(
      DexMethod::get_method(cls->get_type(), clinit_name, clinit_proto));

  if (clinit) {
    return clinit;
  }

  // clinit does not exist, create one
  clinit =
      DexMethod::make_method(cls->get_type(), clinit_name, clinit_proto)
          ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_CONSTRUCTOR, false);

  auto ir_code = std::make_unique<IRCode>(clinit, 1);
  ir_code->push_back(dasm(OPCODE_RETURN_VOID));
  clinit->set_code(std::move(ir_code));
  cls->add_method(clinit);
  return clinit;
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

DexType* get_boxed_reference_type(const DexType* type) {
  switch (type::type_shorty(type)) {
  case 'Z':
    return type::java_lang_Boolean();
  case 'B':
    return type::java_lang_Byte();
  case 'S':
    return type::java_lang_Short();
  case 'C':
    return type::java_lang_Character();
  case 'I':
    return type::java_lang_Integer();
  case 'J':
    return type::java_lang_Long();
  case 'F':
    return type::java_lang_Float();
  case 'D':
    return type::java_lang_Double();
  default:
    return nullptr;
  }
}

// Takes a reference type, returns its corresponding unboxing method
DexMethodRef* get_unboxing_method_for_type(const DexType* type) {
  if (type == type::java_lang_Boolean()) {
    return DexMethod::make_method("Ljava/lang/Boolean;.booleanValue:()Z");
  } else if (type == type::java_lang_Byte()) {
    return DexMethod::make_method("Ljava/lang/Byte;.byteValue:()B");
  } else if (type == type::java_lang_Short()) {
    return DexMethod::make_method("Ljava/lang/Short;.shortValue:()S");
  } else if (type == type::java_lang_Character()) {
    return DexMethod::make_method("Ljava/lang/Character;.charValue:()C");
  } else if (type == type::java_lang_Integer()) {
    return DexMethod::make_method("Ljava/lang/Integer;.intValue:()I");
  } else if (type == type::java_lang_Long()) {
    return DexMethod::make_method("Ljava/lang/Long;.longValue:()J");
  } else if (type == type::java_lang_Float()) {
    return DexMethod::make_method("Ljava/lang/Float;.floatValue:()F");
  } else if (type == type::java_lang_Double()) {
    return DexMethod::make_method("Ljava/lang/Double;.doubleValue:()D");
  }
  return nullptr;
}

// Take a reference type, returns its valueOf function
DexMethodRef* get_value_of_method_for_type(const DexType* type) {
  if (type == type::java_lang_Boolean()) {
    return DexMethod::make_method(
        "Ljava/lang/Boolean;.valueOf:(Z)Ljava/lang/Boolean;");
  } else if (type == type::java_lang_Byte()) {
    return DexMethod::make_method(
        "Ljava/lang/Byte;.valueOf:(B)Ljava/lang/Byte;");
  } else if (type == type::java_lang_Short()) {
    return DexMethod::make_method(
        "Ljava/lang/Short;.valueOf:(S)Ljava/lang/Short;");
  } else if (type == type::java_lang_Character()) {
    return DexMethod::make_method(
        "Ljava/lang/Character;.valueOf:(C)Ljava/lang/Character;");
  } else if (type == type::java_lang_Integer()) {
    return DexMethod::make_method(
        "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;");
  } else if (type == type::java_lang_Long()) {
    return DexMethod::make_method(
        "Ljava/lang/Long;.valueOf:(J)Ljava/lang/Long;");
  } else if (type == type::java_lang_Float()) {
    return DexMethod::make_method(
        "Ljava/lang/Float;.valueOf:(Z)Ljava/lang/Float;");
  } else if (type == type::java_lang_Double()) {
    return DexMethod::make_method(
        "Ljava/lang/Double;.valueOf:(D)Ljava/lang/Double;");
  }
  return nullptr;
}

bool has_hierarchy_in_scope(DexClass* cls) {
  DexType* super = nullptr;
  const DexClass* super_cls = cls;
  while (super_cls) {
    super = super_cls->get_super_class();
    super_cls = type_class_internal(super);
  }
  return super == type::java_lang_Object();
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

void create_runtime_exception_block(DexString* except_str,
                                    std::vector<IRInstruction*>& block) {
  // clang-format off
  // new-instance v0, Ljava/lang/RuntimeException; // type@3852
  // const-string v1, "Exception String e.g. Too many args" // string@7a6d
  // invoke-direct {v0, v1}, Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V
  // throw v0
  // clang-format on
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
  for (const auto& mie : InstructionIterable(code.get_param_instructions())) {
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
  for (fs::directory_iterator it(dexen_dir_path); it != end; ++it) {
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
  auto dex_comparator = [](const fs::path& a, const fs::path& b) {
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
      return bnum > anum;
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
  size_t size{0};
  auto param_ops = code->get_param_instructions();
  for (auto& mie : InstructionIterable(&param_ops)) {
    size += mie.insn->dest_is_wide() ? 2 : 1;
  }
  return size;
}

dex_stats_t& operator+=(dex_stats_t& lhs, const dex_stats_t& rhs) {
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

bool is_uninstantiable_class(DexType* type) {
  if (type == nullptr || type::is_array(type) || type::is_primitive(type)) {
    return false;
  }
  auto cls = type_class(type);
  if (cls == nullptr || is_interface(cls) || is_native(cls) ||
      cls->is_external() || !cls->rstate.can_delete()) {
    return false;
  }
  return !cls->has_ctors();
}

void change_visibility(DexMethod* method, DexType* scope) {
  change_visibility(method->get_code(), scope);
}

void change_visibility(IRCode* code, DexType* scope) {
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
                                               ? FieldSearch::Static
                                               : FieldSearch::Instance);
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
      auto current_method =
          resolve_method(insn->get_method(), opcode_to_search(insn));
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
