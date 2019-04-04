/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ModelMerger.h"

#include "AnnoUtils.h"
#include "ClassAssemblingUtils.h"
#include "DexUtil.h"
#include "MethodReference.h"
#include "PassManager.h"
#include "Resolver.h"
#include "TypeReference.h"
#include "TypeTagUtils.h"
#include "Walkers.h"

#include <sstream>

namespace {

using MergedTypeNames = std::unordered_map<std::string, std::string>;
using CallSites = std::vector<std::pair<DexMethod*, IRInstruction*>>;

TypeTags gen_type_tags(const std::vector<const MergerType*>& mergers) {
  TypeTags res;
  for (auto& merger : mergers) {
    uint32_t val = 0;
    for (const auto type : merger->mergeables) {
      res.set_type_tag(type, val++);
    }
  }
  return res;
}

TypeTags collect_type_tags(const std::vector<const MergerType*>& mergers) {
  TypeTags type_tags;
  for (auto merger : mergers) {
    for (const auto type : merger->mergeables) {
      auto type_tag = type_tag_utils::parse_model_type_tag(type_class(type));
      always_assert_log(
          type_tag != boost::none, "Type tag is missing from %s\n", SHOW(type));
      type_tags.set_type_tag(type, *type_tag);
    }
  }
  return type_tags;
}

DexField* scan_type_tag_field(const char* type_tag_field_name,
                              const DexType* type) {
  DexField* field = nullptr;
  while (field == nullptr && type != get_object_type()) {
    auto cls = type_class(type);
    field = cls->find_field(type_tag_field_name, get_int_type());
    type = cls->get_super_class();
  }

  always_assert_log(field, "Failed to find type tag field!");
  return field;
}

// If no type tags, the result is empty.
MergerToField get_type_tag_fields(
    const std::vector<const DexType*>& model_root_types,
    const std::vector<const MergerType*>& mergers,
    bool input_has_type_tag,
    bool generate_type_tags) {
  MergerToField merger_to_type_tag_field;
  for (const auto model_root_type : model_root_types) {
    for (auto merger : mergers) {
      DexField* field = nullptr;
      if (input_has_type_tag) {
        field =
            scan_type_tag_field(EXTERNAL_TYPE_TAG_FIELD_NAME, model_root_type);
        merger_to_type_tag_field[merger] = field;
      } else if (generate_type_tags) {
        field = scan_type_tag_field(INTERNAL_TYPE_TAG_FIELD_NAME, merger->type);
        merger_to_type_tag_field[merger] = field;
      }
    }
  }
  return merger_to_type_tag_field;
}

void update_code_type_refs(
    const Scope& scope,
    const std::unordered_map<const DexType*, DexType*>& mergeable_to_merger) {
  TRACE(TERA,
        8,
        "  Updating NEW_INSTANCE, NEW_ARRAY, CHECK_CAST & CONST_CLASS\n");
  TypeSet mergeables;
  for (const auto& pair : mergeable_to_merger) {
    mergeables.insert(pair.first);
  }
  auto patcher = [&](DexMethod* meth, IRCode& code) {
    auto ii = InstructionIterable(code);
    for (auto it = ii.begin(); it != ii.end(); ++it) {
      auto insn = it->insn;

      /////////////////////////////////////////////////////
      // Rebind method refs referencing a mergeable to defs
      /////////////////////////////////////////////////////
      if (insn->has_method()) {
        auto meth_ref = insn->get_method();
        if (meth_ref == nullptr || meth_ref->is_def() ||
            meth_ref->is_external() || meth_ref->is_concrete()) {
          continue;
        }
        auto proto = meth_ref->get_proto();
        if (!type_reference::proto_has_reference_to(proto, mergeables)) {
          continue;
        }
        const auto meth_def = resolve_method(meth_ref, opcode_to_search(insn));
        // This is a very tricky case where RebindRefs cannot resolve a
        // MethodRef to MethodDef. It is a invoke-virtual with a MethodRef
        // referencing an interface method implmentation defined in a subclass
        // of the referenced type. To resolve the actual def we need to go
        // through another interface method search. Maybe we should fix it in
        // ReBindRefs.
        if (meth_def == nullptr) {
          auto intf_def = resolve_method(meth_ref, MethodSearch::Interface);
          always_assert(insn->opcode() == OPCODE_INVOKE_VIRTUAL && intf_def);
          auto new_proto = type_reference::update_proto_reference(
              proto, mergeable_to_merger);
          DexMethodSpec spec;
          spec.proto = new_proto;
          meth_ref->change(spec,
                           true /*rename on collision*/,
                           true /*update_deobfuscated_name*/);
          continue;
        }
        always_assert_log(false,
                          "Found mergeable referencing MethodRef %s\n",
                          SHOW(meth_ref));
      }
      ////////////////////////////////////////
      // Update simple type refs
      ////////////////////////////////////////
      if (!insn->has_type()) {
        continue;
      }
      if (insn->opcode() != OPCODE_NEW_INSTANCE &&
          insn->opcode() != OPCODE_CHECK_CAST &&
          insn->opcode() != OPCODE_CONST_CLASS &&
          insn->opcode() != OPCODE_NEW_ARRAY) {
        continue;
      }
      const auto ref_type = insn->get_type();
      auto type = get_array_type_or_self(ref_type);
      if (mergeable_to_merger.count(type) == 0) {
        continue;
      }
      always_assert(type_class(type));
      auto merger_type = mergeable_to_merger.at(type);
      if (is_array(ref_type)) {
        auto array_merger_type = make_array_type(merger_type);
        insn->set_type(array_merger_type);
        TRACE(TERA,
              9,
              "  replacing %s referencing array type of %s\n",
              SHOW(insn),
              SHOW(type));
      } else {
        insn->set_type(const_cast<DexType*>(merger_type));
        TRACE(
            TERA, 9, "  replacing %s referencing %s\n", SHOW(insn), SHOW(type));
      }
    }
  };

  walk::parallel::code(scope, patcher);
}

void update_refs_to_mergeable_fields(
    const Scope& scope,
    const std::vector<const MergerType*>& mergers,
    const std::unordered_map<const DexType*, DexType*>& mergeable_to_merger,
    MergerFields& merger_fields) {
  std::unordered_map<DexField*, DexField*> fields_lookup;
  for (auto& merger : mergers) {
    cook_merger_fields_lookup(
        merger_fields.at(merger->type), merger->field_map, fields_lookup);
  }
  TRACE(TERA, 8, "  Updating field refs\n");
  walk::parallel::code(scope, [&](DexMethod* meth, IRCode& code) {
    auto ii = InstructionIterable(code);
    for (auto it = ii.begin(); it != ii.end(); ++it) {
      auto insn = it->insn;
      if (!insn->has_field()) {
        continue;
      }
      const auto field =
          resolve_field(insn->get_field(),
                        is_ifield_op(insn->opcode()) ? FieldSearch::Instance
                                                     : FieldSearch::Static);
      if (field == nullptr) {
        continue;
      }
      if (fields_lookup.find(field) == fields_lookup.end()) {
        continue;
      }
      const auto new_field = fields_lookup.at(field);
      insn->set_field(new_field);
      TRACE(TERA,
            9,
            "  replacing %s field ref %s (defined on mergeable)\n",
            SHOW(insn),
            SHOW(field));

      if (field->get_type() == new_field->get_type()) {
        continue;
      }
      if (is_iget(insn->opcode())) {
        auto field_type = field->get_type();
        field_type = mergeable_to_merger.count(field_type) > 0
                         ? mergeable_to_merger.at(field_type)
                         : field_type;
        patch_iget(meth, it.unwrap(), field_type);
      } else if (is_iput(insn->opcode())) {
        patch_iput(it.unwrap());
      }
    }
  });
}

DexMethod* create_instanceof_method(const DexType* merger_type,
                                    DexField* type_tag_field) {
  auto arg_list =
      DexTypeList::make_type_list({get_object_type(), get_int_type()});
  auto proto = DexProto::make_proto(get_boolean_type(), arg_list);
  auto access = ACC_PUBLIC | ACC_STATIC;
  auto mc = new MethodCreator(const_cast<DexType*>(merger_type),
                              DexString::make_string(INSTANCE_OF_STUB_NAME),
                              proto,
                              access);
  auto obj_loc = mc->get_local(0);
  auto type_tag_loc = mc->get_local(1);
  // first type check result loc.
  auto check_res_loc = mc->make_local(get_boolean_type());
  auto mb = mc->get_main_block();
  mb->instance_of(obj_loc, check_res_loc, const_cast<DexType*>(merger_type));
  // ret slot.
  auto ret_loc = mc->make_local(get_boolean_type());
  // first check and branch off. Zero means fail.
  auto instance_of_block = mb->if_testz(OPCODE_IF_EQZ, check_res_loc);

  // Fall through. Check succeed.
  auto itype_tag_loc = mc->make_local(get_int_type());
  // CHECK_CAST obj to merger type.
  instance_of_block->check_cast(obj_loc, const_cast<DexType*>(merger_type));
  instance_of_block->iget(type_tag_field, obj_loc, itype_tag_loc);
  // Second type check
  auto tag_match_block =
      instance_of_block->if_test(OPCODE_IF_NE, itype_tag_loc, type_tag_loc);
  // Second check succeed
  tag_match_block->load_const(ret_loc, 1);
  tag_match_block->ret(ret_loc);
  // Fall through, check failed.
  instance_of_block->load_const(ret_loc, 0);
  instance_of_block->ret(ret_loc);

  return mc->create();
}

void update_instance_of(
    const Scope& scope,
    const std::unordered_map<const DexType*, DexType*>& mergeable_to_merger,
    const std::unordered_map<const DexType*, DexMethod*>&
        merger_to_instance_of_meth,
    const TypeTags& type_tags) {
  walk::parallel::code(scope, [&](DexMethod* caller, IRCode& code) {
    auto ii = InstructionIterable(code);
    for (auto it = ii.begin(); it != ii.end(); ++it) {
      auto insn = it->insn;
      if (!insn->has_type() || insn->opcode() != OPCODE_INSTANCE_OF) {
        continue;
      }
      const auto type = insn->get_type();
      if (mergeable_to_merger.count(type) == 0) {
        continue;
      }

      always_assert(type_class(type));
      TRACE(TERA,
            9,
            " patching INSTANCE_OF at %s %s\n",
            SHOW(insn),
            SHOW(caller));
      // Load type_tag.
      auto type_tag = type_tags.get_type_tag(type);
      auto type_tag_reg = code.allocate_temp();
      auto load_type_tag =
          method_reference::make_load_const(type_tag_reg, type_tag);
      // Replace INSTANCE_OF with INVOKE_STATIC to instance_of_meth.
      auto merger_type = mergeable_to_merger.at(type);
      auto instance_of_meth = merger_to_instance_of_meth.at(merger_type);
      std::vector<uint16_t> args;
      args.push_back(insn->src(0));
      args.push_back(type_tag_reg);
      auto invoke = method_reference::make_invoke(
          instance_of_meth, OPCODE_INVOKE_STATIC, args);
      // MOVE_RESULT to dst of INSTANCE_OF.
      auto move_res = new IRInstruction(OPCODE_MOVE_RESULT);
      move_res->set_dest(std::next(it)->insn->dest());
      code.insert_after(
          insn, std::vector<IRInstruction*>{load_type_tag, invoke, move_res});
      // remove original INSTANCE_OF.
      code.remove_opcode(insn);

      TRACE(TERA, 9, " patched INSTANCE_OF in \n%s\n", SHOW(&code));
    }
  });
}

void update_instance_of_no_type_tag(
    const Scope& scope,
    const std::unordered_map<const DexType*, DexType*>& mergeable_to_merger) {
  walk::parallel::code(scope, [&](DexMethod* caller, IRCode& code) {
    auto ii = InstructionIterable(code);
    for (auto it = ii.begin(); it != ii.end(); ++it) {
      auto insn = it->insn;
      if (!insn->has_type() || insn->opcode() != OPCODE_INSTANCE_OF) {
        continue;
      }
      const auto type = insn->get_type();
      if (mergeable_to_merger.count(type) == 0) {
        continue;
      }

      always_assert(type_class(type));
      auto merger_type = mergeable_to_merger.at(type);
      insn->set_type(merger_type);
      TRACE(TERA, 9, " patched INSTANCE_OF no type tag in \n%s\n", SHOW(&code));
    }
  });
}

void update_refs_to_mergeable_types(
    const Scope& scope,
    const std::vector<const MergerType*>& mergers,
    const std::unordered_map<const DexType*, DexType*>& mergeable_to_merger,
    const TypeTags& type_tags,
    const MergerToField& type_tag_fields,
    std::unordered_map<DexMethod*, std::string>& method_debug_map,
    bool has_type_tags) {
  // Update simple type referencing instructions to instantiate merger type.
  update_code_type_refs(scope, mergeable_to_merger);
  type_reference::update_method_signature_type_references(
      scope,
      mergeable_to_merger,
      boost::optional<std::unordered_map<DexMethod*, std::string>&>(
          method_debug_map));
  type_reference::update_field_type_references(scope, mergeable_to_merger);
  // Fix INSTANCE_OF
  if (!has_type_tags) {
    always_assert(type_tag_fields.empty());
    update_instance_of_no_type_tag(scope, mergeable_to_merger);
    return;
  }
  std::unordered_map<const DexType*, DexMethod*> merger_to_instance_of_meth;
  for (auto merger : mergers) {
    auto type = merger->type;
    auto type_tag_field = type_tag_fields.at(merger);
    auto instance_of_meth = create_instanceof_method(type, type_tag_field);
    merger_to_instance_of_meth[type] = instance_of_meth;
    type_class(type)->add_method(instance_of_meth);
  }
  update_instance_of(
      scope, mergeable_to_merger, merger_to_instance_of_meth, type_tags);
}

void update_const_string_type_refs(const Scope& scope,
                                   const MergedTypeNames& merged_type_names) {
  // Rewrite all const-string strings for merged classes.
  walk::parallel::code(scope, [&](DexMethod* meth, IRCode& code) {
    for (const auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;

      if (insn->opcode() != OPCODE_CONST_STRING) {
        continue;
      }

      DexString* dex_str = insn->get_string();
      const std::string& internal_str =
          JavaNameUtil::external_to_internal(dex_str->str());

      const auto& find_name_to = merged_type_names.find(internal_str);
      if (find_name_to != merged_type_names.end()) {
        const std::string& name_to = find_name_to->second;
        DexString* dex_name_to =
            DexString::make_string(JavaNameUtil::internal_to_external(name_to));
        insn->set_string(dex_name_to);
        TRACE(TERA,
              8,
              "Replace const-string from %s to %s\n",
              dex_str->c_str(),
              dex_name_to->c_str());
      }
    }
  });
}

std::string merger_info(const MergerType& merger) {
  std::ostringstream ss;
  ss << " assembling merger " << SHOW(merger.type) << " - mergeables "
     << merger.mergeables.size() << ", dmethods " << merger.dmethods.size()
     << ", non_virt_methods " << merger.non_virt_methods.size()
     << ", virt_methods " << merger.vmethods.size() << "\n";
  for (const auto imeths : merger.intfs_methods) {
    ss << "  interface methods " << imeths.methods.size() << "\n";
  }
  ss << " Field maps \n";
  for (auto fmap : merger.field_map) {
    ss << "  type " << SHOW(fmap.first) << "\n";
    size_t num_empty_fields = 0;
    for (const auto field : fmap.second) {
      if (field != nullptr) {
        ss << "    field " << field->c_str() << " " << SHOW(field->get_type())
           << "\n";
      } else {
        ss << "    field -- empty due to imprecise shaping\n";
        num_empty_fields++;
      }
    }
    ss << "    Total empty fields = " << num_empty_fields << "\n";
  }
  return ss.str();
}

void set_interfaces(DexClass* cls, const TypeSet& intfs) {
  if (!intfs.empty()) {
    auto intf_list = std::deque<DexType*>();
    for (const auto& intf : intfs) {
      intf_list.emplace_back(const_cast<DexType*>(intf));
    }
    auto new_intfs = DexTypeList::make_type_list(std::move(intf_list));
    cls->set_interfaces(new_intfs);
  }
};

void fix_existing_merger_cls(const Model& model,
                             const MergerType& merger,
                             DexClass* cls,
                             DexType* type) {
  always_assert_log(
      !cls->is_external(), "%s and must be an internal DexClass", SHOW(type));
  always_assert_log(merger.mergeables.empty(),
                    "%s cannot have mergeables",
                    merger_info(merger).c_str());
  const auto& intfs = model.get_interfaces(type);
  set_interfaces(cls, intfs);
  cls->set_super_class(const_cast<DexType*>(model.get_parent(type)));
  if (merger.kill_fields) {
    for (const auto& field : cls->get_ifields()) {
      cls->remove_field(field);
    }
  }
  TRACE(TERA,
        5,
        "create hierarhcy: updated DexClass from MergerType: %s\n",
        SHOW(cls));
}

// Trim the debug map to only contain mergeable methods.
void trim_method_debug_map(
    const std::unordered_map<const DexType*, DexType*>& mergeable_to_merger,
    std::unordered_map<DexMethod*, std::string>& method_debug_map) {
  TRACE(TERA, 5, "Method debug map un-trimmed %d\n", method_debug_map.size());
  size_t trimmed_cnt = 0;
  for (auto it = method_debug_map.begin(); it != method_debug_map.end();) {
    auto owner_type = it->first->get_class();
    if (mergeable_to_merger.count(owner_type) == 0) {
      it = method_debug_map.erase(it);
      ++trimmed_cnt;
    } else {
      ++it;
    }
  }

  TRACE(TERA, 5, "Method debug map trimmed %d\n", trimmed_cnt);
}

void write_out_type_mapping(
    const std::unordered_map<const DexType*, DexType*>& mergeable_to_merger,
    const TypeToMethodMap& method_dedup_map,
    const std::string& mapping_file) {
  std::ofstream os(mapping_file, std::ios_base::app);
  if (!os.is_open()) {
    return;
  }

  std::ostringstream out;
  for (const auto& pair : mergeable_to_merger) {
    auto mergeable = pair.first;
    auto merger = pair.second;
    out << SHOW(mergeable) << " -> " << SHOW(merger) << std::endl;

    for (auto& symbol_map : method_dedup_map.at(mergeable)) {
      out << "  " << symbol_map.first << " -> " << SHOW(symbol_map.second)
          << std::endl;
    }
  }
  out << std::endl;

  os << out.str();
  TRACE(TERA,
        4,
        "Dumped type mapping (%d) to %s\n",
        out.str().size(),
        mapping_file.c_str());
}

} // namespace

const std::vector<DexField*> ModelMerger::empty_fields =
    std::vector<DexField*>();

void ModelMerger::update_merger_fields(const MergerType& merger) {
  auto merger_fields =
      merger.has_fields()
          ? create_merger_fields(merger.type, merger.field_map.begin()->second)
          : empty_fields;
  m_merger_fields[merger.type] = merger_fields;
}

void ModelMerger::update_stats(const std::string model_name,
                               const std::vector<const MergerType*>& mergers,
                               ModelMethodMerger& mm) {
  for (auto merger : mergers) {
    m_stats.m_num_classes_merged += merger->mergeables.size();
  }
  // Print method stats
  mm.print_method_stats(model_name, m_stats.m_num_classes_merged);
  m_stats += mm.get_stats();
}

std::vector<DexClass*> ModelMerger::merge_model(
    Scope& scope,
    DexStoresVector& stores,
    Model& model,
    boost::optional<size_t> max_num_dispatch_target) {
  Timer t("merge_model");
  std::vector<const MergerType*> to_materialize;
  std::vector<DexClass*> merger_classes;
  MergedTypeNames merged_type_names;
  const auto model_spec = model.get_model_spec();
  bool input_has_type_tag = model_spec.input_has_type_tag();

  model.walk_hierarchy([&](const MergerType& merger) {
    // a model hierarchy is walked top down BFS style.
    // The walker is passed only merger that need computation.
    // A MergerType may or may not have mergeables.
    // A MergerType may or may not have a DexClass, if not one has
    // to be created.
    // A set of properties in the MergerType define the operation to
    // perform on the given type.

    DexType* type = const_cast<DexType*>(merger.type);
    auto cls = type_class(type);
    const auto& intfs = model.get_interfaces(type);
    TRACE(TERA, 3, "%s", merger_info(merger).c_str());

    // MergerType has an existing class, update interfaces,
    // fields and parent
    if (cls != nullptr) {
      fix_existing_merger_cls(model, merger, cls, type);
      return;
    }

    update_merger_fields(merger);
    cls = create_merger_class(type,
                              model.get_parent(type),
                              m_merger_fields.at(type),
                              intfs,
                              model_spec.generate_type_tag(),
                              !merger.has_mergeables());
    // TODO: replace this with an annotation.
    cls->rstate.set_interdex_subgroup(merger.interdex_subgroup);
    cls->rstate.set_generated();

    add_class(cls, scope, stores);
    merger_classes.push_back(cls);

    if (!merger.has_mergeables()) {
      return;
    }
    // Bail out if we should not generate type tags and there are vmethods
    // or intfs_methods.
    if (model_spec.no_type_tag()) {
      if (merger.vmethods.size() || merger.intfs_methods.size()) {
        TRACE(TERA,
              5,
              "Bailing out: no type tag merger %s w/ true virtuals\n",
              SHOW(merger.type));
        return;
      }
    }
    to_materialize.emplace_back(&merger);
  });

  // Merging transformations.
  std::unordered_map<const DexType*, DexType*> mergeable_to_merger;
  for (auto merger : to_materialize) {
    auto type = const_cast<DexType*>(merger->type);
    for (auto mergeable : merger->mergeables) {
      merged_type_names[mergeable->get_name()->str()] = type->get_name()->str();
      mergeable_to_merger[mergeable] = type;
    }
  }

  TypeTags type_tags = input_has_type_tag ? collect_type_tags(to_materialize)
                                          : gen_type_tags(to_materialize);
  auto type_tag_fields = get_type_tag_fields(model.get_roots(),
                                             to_materialize,
                                             input_has_type_tag,
                                             model_spec.generate_type_tag());
  std::unordered_map<DexMethod*, std::string> method_debug_map;
  update_refs_to_mergeable_types(scope,
                                 to_materialize,
                                 mergeable_to_merger,
                                 type_tags,
                                 type_tag_fields,
                                 method_debug_map,
                                 model_spec.has_type_tag());
  trim_method_debug_map(mergeable_to_merger, method_debug_map);
  update_refs_to_mergeable_fields(
      scope, to_materialize, mergeable_to_merger, m_merger_fields);

  // Merge methods
  ModelMethodMerger mm(scope,
                       to_materialize,
                       type_tag_fields,
                       &type_tags,
                       method_debug_map,
                       model.get_model_spec(),
                       max_num_dispatch_target);
  auto mergeable_to_merger_ctor = mm.merge_methods();
  update_stats(model.get_name(), to_materialize, mm);
  update_const_string_type_refs(scope, merged_type_names);

  // Write out mapping files
  auto method_dedup_map = mm.get_method_dedup_map();
  write_out_type_mapping(mergeable_to_merger, method_dedup_map, s_mapping_file);
  if (!to_materialize.empty()) {
    post_process(model, type_tags, mergeable_to_merger_ctor);
  }

  TRACE(TERA, 3, "created %d merger classes\n", merger_classes.size());
  m_stats.m_num_generated_classes = merger_classes.size();
  return merger_classes;
}

void ModelMerger::update_redex_stats(const std::string& prefix,
                                     PassManager& mgr) const {
  mgr.incr_metric((prefix + "_merger_class_generated").c_str(),
                  m_stats.m_num_generated_classes);
  mgr.incr_metric((prefix + "_class_merged").c_str(),
                  m_stats.m_num_classes_merged);
  mgr.incr_metric((prefix + "_ctor_dedupped").c_str(),
                  m_stats.m_num_ctor_dedupped);
  mgr.incr_metric((prefix + "_static_non_virt_dedupped").c_str(),
                  m_stats.m_num_static_non_virt_dedupped);
  mgr.incr_metric((prefix + "_vmethods_dedupped").c_str(),
                  m_stats.m_num_vmethods_dedupped);
  mgr.set_metric((prefix + "_const_lifted_methods").c_str(),
                 m_stats.m_num_const_lifted_methods);
  mgr.incr_metric((prefix + "_merged_static_methods").c_str(),
                  m_stats.m_num_merged_static_methods);
  mgr.incr_metric((prefix + "_merged_direct_methods").c_str(),
                  m_stats.m_num_merged_direct_methods);
  mgr.incr_metric((prefix + "_merged_nonvirt_methods").c_str(),
                  m_stats.m_num_merged_nonvirt_methods);
}
