/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OptimizeResources.h"

#include <algorithm>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/regex.hpp>
#include <json/value.h>
#include <unordered_set>

#include "ApkResources.h"
#include "BundleResources.h"
#include "ConfigFiles.h"
#include "DetectBundle.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "GlobalConfig.h"
#include "IOUtil.h"
#include "IRInstruction.h"
#include "LiveRange.h"
#include "PassManager.h"
#include "ReachableClasses.h"
#include "RedexMappedFile.h"
#include "RedexResources.h"
#include "Resolver.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "StlUtil.h"
#include "TrackResources.h"
#include "Walkers.h"
#include "androidfw/ResourceTypes.h"
#include "utils/Vector.h"

namespace opt_res {
ReachableResourcesPlugin::ReachableResourcesPlugin(const std::string& name)
    : m_name(name) {
  ReachableResourcesPluginRegistry::get().register_plugin(this);
}

ReachableResourcesPluginRegistry& ReachableResourcesPluginRegistry::get() {
  static ReachableResourcesPluginRegistry* registry =
      new ReachableResourcesPluginRegistry();
  return *registry;
  // static ReachableResourcesPluginRegistry registry;
  // return registry;
}

void ReachableResourcesPluginRegistry::register_plugin(
    ReachableResourcesPlugin* plugin) {
  m_registered_plugins.push_back(plugin);
}

const std::vector<ReachableResourcesPlugin*>&
ReachableResourcesPluginRegistry::get_plugins() const {
  return m_registered_plugins;
}
} // namespace opt_res

namespace {

bool is_resource_class_name(const std::string_view c_name) {
  return c_name.find("/R$") != std::string::npos;
}

bool is_resource_class(const DexClass* cls) {
  const auto c_name = cls->get_name()->str();
  const auto d_name = cls->get_deobfuscated_name_or_empty();
  return is_resource_class_name(c_name) || is_resource_class_name(d_name);
}

std::vector<std::string> get_resource_classes(const Scope& scope) {
  std::vector<std::string> resource_classes;
  for (const DexClass* clazz : scope) {
    const auto name_str = clazz->get_name()->str();
    if (is_resource_class(clazz)) {
      resource_classes.push_back(str_copy(name_str));
    }
  }

  return resource_classes;
}

void extract_resources_from_static_arrays(
    const std::unordered_set<DexField*>& array_fields,
    std::unordered_set<uint32_t>& array_values) {
  std::unordered_set<DexClass*> classes_to_search;
  for (DexField* field : array_fields) {
    DexClass* clazz = type_class(field->get_class());

    // We can assert a non-null class since we know these fields came
    // from classes we iterated over.
    always_assert(clazz != nullptr);
    classes_to_search.emplace(clazz);
  }

  for (auto clazz : classes_to_search) {
    DexMethod* clinit = clazz->get_clinit();
    IRCode* ir_code = clinit->get_code();
    if (ir_code == nullptr) {
      continue;
    }
    cfg::ScopedCFG cfg(ir_code);
    live_range::MoveAwareChains move_aware_chains(*cfg);
    auto use_defs = move_aware_chains.get_use_def_chains();
    auto def_uses = move_aware_chains.get_def_use_chains();
    for (const auto& mie : InstructionIterable(*cfg)) {
      auto insn = mie.insn;
      if (!opcode::is_an_sput(insn->opcode())) {
        continue;
      }
      auto field = resolve_field(insn->get_field(), FieldSearch::Static);
      // Only consider array values for the requested fields.
      if (!array_fields.count(field)) {
        continue;
      }
      std::unordered_set<uint32_t> inner_array_values;
      auto& array_defs = use_defs.at(live_range::Use{insn, 0});
      // should be only one, but we can be conservative and consider all
      for (auto* array_def : array_defs) {
        auto& uses = def_uses.at(array_def);
        if (array_def->opcode() == OPCODE_SGET_OBJECT && uses.size() == 1) {
          continue;
        }
        always_assert_log(array_def->opcode() == OPCODE_NEW_ARRAY,
                          "OptimizeResources does not support extracting "
                          "resources from array created by %s\nin %s:\n%s",
                          SHOW(array_def), SHOW(clinit), SHOW(*cfg));
        // should be only one, but we can be conservative and consider all
        for (auto& use : uses) {
          switch (use.insn->opcode()) {
          case OPCODE_FILL_ARRAY_DATA: {
            always_assert(use.src_index == 0);
            auto array_ints =
                get_fill_array_data_payload<uint32_t>(use.insn->get_data());
            for (uint32_t entry_x : array_ints) {
              if (entry_x > PACKAGE_RESID_START) {
                inner_array_values.emplace(entry_x);
              }
            }
            break;
          }
          case OPCODE_APUT: {
            always_assert(use.src_index == 1);
            auto value_defs = use_defs.at(live_range::Use{use.insn, 0});
            for (auto* value_def : value_defs) {
              always_assert_log(value_def->opcode() == OPCODE_CONST,
                                "OptimizeResources does not support extracting "
                                "resources from value given by %s",
                                SHOW(value_def));
              auto const_literal = value_def->get_literal();
              if (const_literal > PACKAGE_RESID_START) {
                inner_array_values.emplace(const_literal);
              }
            }
            break;
          }
          default:
            if (use.insn != insn) {
              always_assert_log(false,
                                "OptimizeResources does not support extracting "
                                "resources from array escaping via %s",
                                SHOW(use.insn));
            }
            break;
          }
        }
      }
      array_values.insert(inner_array_values.begin(), inner_array_values.end());
    }
  }
}

namespace {
// Return true if the given string is a relative file path, has .xml extension
// and can refer to the res directory of an .apk or .aab file.
bool is_resource_xml(const std::string& str) {
  if (boost::algorithm::ends_with(str, ".xml")) {
    boost::filesystem::path p(str);
    if (p.is_relative()) {
      auto it = p.begin();
      if (it->string() == RES_DIRECTORY) {
        return true;
      }
      if (it != p.end() && (++it)->string() == RES_DIRECTORY) {
        return true;
      }
    }
  }
  return false;
}
} // namespace

void compute_transitive_closure(
    ResourceTableFile* res_table,
    const std::string& zip_dir,
    AndroidResources* resources,
    const std::unordered_set<uint32_t>& accessible_id_roots,
    std::unordered_set<uint32_t>* nodes_visited,
    std::unordered_set<std::string>* explored_xml_files) {
  std::unordered_set<std::string> potential_file_paths;
  for (uint32_t root : accessible_id_roots) {
    res_table->walk_references_for_resource(
        root, ResourcePathType::ZipPath, nodes_visited, &potential_file_paths);
  }

  std::unordered_set<std::string> next_xml_files;
  while (!potential_file_paths.empty()) {
    for (auto& str : potential_file_paths) {
      if (is_resource_xml(str)) {
        auto r_str = zip_dir + "/" + str;
        if (explored_xml_files->find(r_str) == explored_xml_files->end()) {
          next_xml_files.emplace(r_str);
        }
      }
    }

    potential_file_paths.clear();
    for (auto& str : next_xml_files) {
      explored_xml_files->emplace(str);
      for (uint32_t attribute : resources->get_xml_reference_attributes(str)) {
        res_table->walk_references_for_resource(
            attribute, ResourcePathType::ZipPath, nodes_visited,
            &potential_file_paths);
      }
    }
    next_xml_files.clear();
  }

  TRACE(OPTRES, 2, "nodes_visited count: %zu", nodes_visited->size());
  TRACE(OPTRES, 2, "explored_xml_files count: %zu", explored_xml_files->size());
}

/*
 * Delete unvisited res ids from res table.
 */
std::unordered_set<uint32_t> delete_unvisited_resources(
    const std::map<uint32_t, std::string>& id_to_name,
    const std::unordered_set<uint32_t>& nodes_visited,
    ResourceTableFile* table,
    std::unordered_set<std::string>* out_files_to_delete) {
  std::unordered_set<uint32_t> deleted_resources;
  std::unordered_set<std::string> files_to_keep;
  for (auto& p : id_to_name) {
    if (nodes_visited.find(p.first) == nodes_visited.end()) {
      // Collect any res/ files we can now delete. This will influence
      // reachability of Java classes. When handling an .aab input, resolve the
      // on-device file paths to their path relative to unpack dir.
      const auto& files =
          table->get_files_by_rid(p.first, ResourcePathType::ZipPath);
      for (const auto& file_path : files) {
        TRACE(OPTRES, 5, "Delete candidate file %s for unused res ID 0x%x (%s)",
              file_path.c_str(), p.first, p.second.c_str());
        out_files_to_delete->emplace(file_path);
      }
      deleted_resources.emplace(p.first);
      table->delete_resource(p.first);
    } else {
      const auto& files =
          table->get_files_by_rid(p.first, ResourcePathType::ZipPath);
      for (const auto& file_path : files) {
        TRACE(OPTRES, 5, "file to keep %s for reachable res ID 0x%x (%s)",
              file_path.c_str(), p.first, p.second.c_str());
        files_to_keep.emplace(file_path);
      }
    }
  }
  std20::erase_if(*out_files_to_delete, [&](const auto& filename) {
    return files_to_keep.find(filename) != files_to_keep.end();
  });
  return deleted_resources;
}

std::map<uint32_t, uint32_t> build_remapping(
    const std::vector<uint32_t>& sorted_res_ids,
    const std::map<uint32_t, std::string>& id_to_name,
    const std::unordered_set<uint32_t>& deleted_resources,
    const std::string& out_file) {
  std::map<uint32_t, uint32_t> kept_to_remapped_ids;
  uint32_t current_type = 0;
  int subtrahend_for_current_type = 0;
  Json::Value map_json;
  for (size_t index = 0; index < sorted_res_ids.size(); ++index) {
    uint32_t id = sorted_res_ids[index];
    constexpr const int PACKAGE_IDENTIFIER_MASK = 0xFF000000;
    uint32_t package_id = id & PACKAGE_IDENTIFIER_MASK;
    always_assert(package_id == PACKAGE_RESID_START);
    constexpr const int TYPE_IDENTIFIER_MASK = 0x00FF0000;
    uint32_t type_id = id & TYPE_IDENTIFIER_MASK;
    if (type_id != current_type) {
      subtrahend_for_current_type = 0;
      current_type = type_id;
    }

    Json::Value json_row;
    json_row["old_id"] = (boost::format("%x") % id).str();
    json_row["name"] = id_to_name.at(id);

    if (deleted_resources.find(id) != deleted_resources.end()) {
      subtrahend_for_current_type++;
      json_row["new_id"] = "(del)";
    } else {
      kept_to_remapped_ids[id] = id - subtrahend_for_current_type;
      json_row["new_id"] =
          (boost::format("%x") % kept_to_remapped_ids[id]).str();
    }

    map_json.append(json_row);
  }

  write_string_to_file(out_file, map_json.toStyledString());

  return kept_to_remapped_ids;
}

std::unordered_set<uint32_t> get_disallowed_resources(
    const std::vector<uint32_t>& sorted_res_ids,
    const std::unordered_set<uint32_t>& disallowed_types) {
  constexpr const int TYPE_IDENTIFIER_MASK = 0x00FF0000;
  std::unordered_set<uint32_t> disallowed_resources;

  for (size_t index = 0; index < sorted_res_ids.size(); ++index) {
    uint32_t id = sorted_res_ids[index];
    uint32_t type_id = id & TYPE_IDENTIFIER_MASK;
    if (disallowed_types.count(type_id)) {
      disallowed_resources.emplace(id);
    }
  }

  return disallowed_resources;
}

std::unordered_set<uint32_t> get_resources_by_name_prefix(
    const std::vector<std::string>& prefixes,
    const std::map<std::string, std::vector<uint32_t>>& name_to_ids) {
  std::unordered_set<uint32_t> found_resources;

  for (const auto& pair : name_to_ids) {
    for (const auto& prefix : prefixes) {
      if (boost::algorithm::starts_with(pair.first, prefix)) {
        found_resources.insert(pair.second.begin(), pair.second.end());
      }
    }
  }

  return found_resources;
}

} // namespace

void OptimizeResourcesPass::report_metric(TraceModule trace_module,
                                          const std::string& metric_name,
                                          int metric_value,
                                          PassManager& mgr) {
  TRACE(trace_module, 1, "%s: %d", metric_name.c_str(), metric_value);
  mgr.set_metric(metric_name, metric_value);
}

void OptimizeResourcesPass::remap_resource_classes(
    DexStoresVector& stores,
    const std::map<uint32_t, uint32_t>& old_to_remapped_ids) {
  int replaced_fields = 0;
  auto scope = build_class_scope(stores);
  for (auto clazz : scope) {
    if (is_resource_class(clazz)) {
      const std::vector<DexField*>& fields = clazz->get_sfields();
      for (auto& field : fields) {
        uint64_t f_value = field->get_static_value()->value();
        const uint32_t MAX_UINT_32 = 0xFFFFFFFF;

        // f_value is a uint64_t, but we know it will be safe to cast down to
        // a uint32_t since it will be a resource ID- hence, we assert.
        always_assert(f_value <= MAX_UINT_32);
        if (f_value > PACKAGE_RESID_START &&
            old_to_remapped_ids.count(f_value)) {
          field->get_static_value()->value(
              (uint64_t)old_to_remapped_ids.at((uint32_t)f_value));
          ++replaced_fields;
        }
      }
    }
  }

  TRACE(OPTRES, 2, "replaced_fields count: %d", replaced_fields);
}

void remap_resource_class_clinit(
    const DexClass* cls,
    const std::map<uint32_t, uint32_t>& old_to_remapped_ids,
    DexMethod* clinit) {
  const auto c_name = cls->get_name()->str();
  IRCode* ir_code = clinit->get_code();

  // Lookup from new-array instruction to an updated array size.
  std::unordered_map<IRInstruction*, uint32_t> new_array_size_updates;
  IRInstruction* last_new_array = nullptr;

  for (const MethodItemEntry& mie : InstructionIterable(ir_code)) {
    IRInstruction* insn = mie.insn;
    if (insn->opcode() == OPCODE_CONST) {
      auto const_literal = insn->get_literal();
      if (const_literal > PACKAGE_RESID_START) {
        auto remapped_literal = old_to_remapped_ids.find(const_literal);
        const uint32_t MAX_UINT_32 = 0xFFFFFFFF;
        // const_literal is a int64_t, but we know it will be safe to cast
        // down to a uint32_t since it will be a resource ID-
        // hence, we assert.
        always_assert(const_literal <= MAX_UINT_32);
        if (remapped_literal != old_to_remapped_ids.end()) {
          insn->set_literal(remapped_literal->second);
        }
      }
    } else if (insn->opcode() == OPCODE_NEW_ARRAY) {
      last_new_array = insn;
    } else if (insn->opcode() == OPCODE_FILL_ARRAY_DATA) {
      DexOpcodeData* op_data = insn->get_data();
      always_assert(op_data->size() > 3);
      always_assert_log(last_new_array != nullptr, "new-array not found");
      std::vector<uint16_t> filtered_op_data_entries;

      uint16_t header_entry = *(op_data->data());
      int deleted_array_element_count = 0;
      auto array_ints = get_fill_array_data_payload<uint32_t>(op_data);
      for (uint32_t entry_x : array_ints) {
        if (entry_x > PACKAGE_RESID_START) {
          bool keep = old_to_remapped_ids.count(entry_x);
          if (keep) {
            const uint16_t* remapped_entry_parts =
                (uint16_t*)&(old_to_remapped_ids.at(entry_x));
            filtered_op_data_entries.push_back(*remapped_entry_parts++);
            filtered_op_data_entries.push_back(*remapped_entry_parts);
          } else {
            // For styleable, we avoid actually deleting entries since
            // there are offsets that will point to the wrong positions
            // in the array. Instead, we zero out the values.
            if (c_name.find("R$styleable") != std::string::npos) {
              filtered_op_data_entries.push_back(0);
              filtered_op_data_entries.push_back(0);
            } else {
              ++deleted_array_element_count;
            }
          }
        } else {
          const uint16_t* original_entry_parts = (uint16_t*)&(entry_x);
          filtered_op_data_entries.push_back(*original_entry_parts++);
          filtered_op_data_entries.push_back(*original_entry_parts);
        }
      }

      uint32_t new_size = filtered_op_data_entries.size() / 2;
      uint16_t* size_pieces = ((uint16_t*)&new_size);

      if (new_size != array_ints.size()) {
        new_array_size_updates.emplace(last_new_array, new_size);
      }

      filtered_op_data_entries.insert(filtered_op_data_entries.begin(),
                                      size_pieces[1]);
      filtered_op_data_entries.insert(filtered_op_data_entries.begin(),
                                      size_pieces[0]);
      filtered_op_data_entries.insert(filtered_op_data_entries.begin(),
                                      header_entry);
      filtered_op_data_entries.insert(filtered_op_data_entries.begin(),
                                      FOPCODE_FILLED_ARRAY);

      op_data = new DexOpcodeData(filtered_op_data_entries.data(),
                                  filtered_op_data_entries.size() - 1);
      insn->set_data(op_data);
    }
  }
  // If any array entries were deleted we need to update the new-array constant.
  // Do this by inserting new instructions (creating dead/non-optimal code that
  // should get cleaned by a later pass).
  if (new_array_size_updates.empty()) {
    return;
  }
  auto ii = InstructionIterable(ir_code);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    IRInstruction* insn = it->insn;
    auto search = new_array_size_updates.find(insn);
    if (search != new_array_size_updates.end()) {
      auto new_size = search->second;
      TRACE(OPTRES, 3, "Updating new-array size %u at instruction: %s",
            new_size, SHOW(*it));
      // Make this additive to not impact other instructions.
      // Note that the naive numbering scheme here requires RegAlloc to be run
      // later (which should be the case for all apps).
      auto new_reg = ir_code->allocate_temp();
      auto const_insn = new IRInstruction(OPCODE_CONST);
      const_insn->set_literal(new_size);
      const_insn->set_dest(new_reg);
      insn->set_src(0, new_reg);
      ir_code->insert_before(it.unwrap(), const_insn);
    }
  }
}

void OptimizeResourcesPass::remap_resource_class_arrays(
    DexStoresVector& stores,
    const GlobalConfig& global_config,
    const std::map<uint32_t, uint32_t>& old_to_remapped_ids) {
  auto global_resources_config =
      global_config.get_config_by_name<ResourceConfig>("resources");
  remap_resource_class_arrays(stores, *global_resources_config,
                              old_to_remapped_ids);
}

void OptimizeResourcesPass::remap_resource_class_arrays(
    DexStoresVector& stores,
    const ResourceConfig& global_resources_config,
    const std::map<uint32_t, uint32_t>& old_to_remapped_ids) {
  auto scope = build_class_scope(stores);
  for (auto clazz : scope) {
    const auto c_name = clazz->get_name()->str_copy();

    if (global_resources_config.customized_r_classes.count(c_name) > 0 ||
        c_name.find("R$styleable") != std::string::npos) {
      DexMethod* clinit = clazz->get_clinit();
      if (clinit == nullptr) {
        continue;
      }
      TRACE(OPTRES, 2, "remap_resource_class_arrays, class %s", SHOW(clazz));
      IRCode* ir_code = clinit->get_code();
      if (ir_code == nullptr) {
        continue;
      }
      remap_resource_class_clinit(clazz, old_to_remapped_ids, clinit);
    }
  }
}

bool is_potential_resid(int64_t id) {
  return id >= 0x7f000000 && id <= 0x7fffffff;
}

std::unordered_set<uint32_t>
OptimizeResourcesPass::find_code_resource_references(
    DexStoresVector& stores,
    ConfigFiles& conf,
    PassManager& mgr,
    const std::map<std::string, std::vector<uint32_t>>& name_to_ids,
    bool check_string_for_name,
    bool assume_id_inlined) {
  std::unordered_set<uint32_t> ids_from_code;
  Scope scope = build_class_scope(stores);

  std::unordered_set<DexField*> recorded_fields;
  const auto& pg_map = conf.get_proguard_map();

  std::vector<std::string> resource_classes = get_resource_classes(scope);

  auto tracked_classes =
      TrackResourcesPass::build_tracked_cls_set(resource_classes, pg_map);
  std::unordered_set<std::string> search_all_classes;
  auto num_field_references = TrackResourcesPass::find_accessed_fields(
      scope, tracked_classes, search_all_classes, &recorded_fields);
  mgr.incr_metric("num_field_references", num_field_references);

  std::unordered_set<DexField*> array_fields;
  for (auto& field : recorded_fields) {
    // Ignore fields with non-resID values
    if (type::is_primitive(field->get_type()) &&
        field->get_static_value()->value() > PACKAGE_RESID_START) {
      ids_from_code.emplace(field->get_static_value()->value());
    }

    if (type::is_array(field->get_type())) {
      array_fields.emplace(field);
    }
  }

  std::unordered_set<uint32_t> array_values_referenced;
  // TODO: Improve this piece of analysis to lift restriction of having to run
  // the pass before ReduceArrayLiteralsPass and
  // InstructionSequenceOutlinerPass.
  extract_resources_from_static_arrays(array_fields, array_values_referenced);

  TRACE(OPTRES, 2, "array_values_referenced count: %zu",
        array_values_referenced.size());
  ids_from_code.insert(array_values_referenced.begin(),
                       array_values_referenced.end());

  if (assume_id_inlined) {
    // Let's walk through code and collect all values that looks like a
    // resource id.
    ConcurrentSet<uint32_t> potential_ids_from_code;
    ConcurrentSet<DexField*> accessed_sfields;
    boost::regex find_ints("(\\d+)");
    walk::parallel::opcodes(scope, [&](DexMethod*, IRInstruction* insn) {
      if (insn->has_literal()) {
        auto lit = insn->get_literal();
        if (is_potential_resid(lit)) {
          potential_ids_from_code.emplace(lit);
        }
      } else if (insn->has_string()) {
        // Redex evaluates expressions like
        // String.valueOf(R.drawable.inspiration_no_format)
        // which means we need to parse ints encoded as strings or ints that
        // were constant folded/concatenated at build time with other strings.
        std::string to_find = insn->get_string()->str_copy();
        std::vector<std::string> int_strings;
        std::copy(boost::sregex_token_iterator(to_find.begin(), to_find.end(),
                                               find_ints),
                  boost::sregex_token_iterator(),
                  std::back_inserter(int_strings));
        for (const auto& int_string : int_strings) {
          int64_t potential_num;
          try {
            potential_num = std::stol(int_string);
          } catch (...) {
            continue;
          }
          if (is_potential_resid(potential_num)) {
            potential_ids_from_code.emplace(potential_num);
          }
        }
      } else if (insn->has_field() && opcode::is_an_sfield_op(insn->opcode())) {
        auto field = resolve_field(insn->get_field(), FieldSearch::Static);
        if (field && field->is_concrete()) {
          accessed_sfields.emplace(field);
        }
      }
    });

    for (auto* field : accessed_sfields) {
      // Ignore fields with non-resID values
      if (type::is_primitive(field->get_type()) && field->get_static_value() &&
          is_potential_resid(field->get_static_value()->value())) {
        ids_from_code.emplace(field->get_static_value()->value());
      }
    }
    ids_from_code.insert(potential_ids_from_code.begin(),
                         potential_ids_from_code.end());
  }
  if (check_string_for_name) {
    ConcurrentSet<uint32_t> potential_ids_from_strings;
    walk::parallel::opcodes(scope, [&](DexMethod*, IRInstruction* insn) {
      if (insn->has_string()) {
        // Being more conservative of what might get passed into
        // Landroid/content/res/Resources;.getIdentifier:(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I
        std::string to_find = insn->get_string()->str_copy();
        auto search = name_to_ids.find(to_find);
        if (search != name_to_ids.end()) {
          potential_ids_from_strings.insert(search->second.begin(),
                                            search->second.end());
        }
      }
    });
    ids_from_code.insert(potential_ids_from_strings.begin(),
                         potential_ids_from_strings.end());
  }
  return ids_from_code;
}

void OptimizeResourcesPass::eval_pass(DexStoresVector& stores,
                                      ConfigFiles& conf,
                                      PassManager&) {
  auto& plugin_registery = opt_res::ReachableResourcesPluginRegistry::get();
  plugin_registery.sort();
  for (auto& p : plugin_registery.get_plugins()) {
    p->configure(conf);
  }
}

void OptimizeResourcesPass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& conf,
                                     PassManager& mgr) {
  std::string zip_dir;
  conf.get_json_config().get("apk_dir", "", zip_dir);
  always_assert(zip_dir.size());

  // 1. Get all known resource ID's from either resources.pb(AAB) or
  // resources.arsc(APK) file.
  auto resources = create_resource_reader(zip_dir);
  auto res_table = resources->load_res_table();

  // 2. Get all resources directly referenced by source code.
  std::unordered_set<uint32_t> ids_from_code = find_code_resource_references(
      stores, conf, mgr, res_table->name_to_ids, m_check_string_for_name,
      m_assume_id_inlined);
  const auto& sorted_res_ids = res_table->sorted_res_ids;
  std::unordered_set<uint32_t> existing_resids;
  for (size_t index = 0; index < sorted_res_ids.size(); ++index) {
    existing_resids.emplace(sorted_res_ids[index]);
  }
  std20::erase_if(ids_from_code, [&](const auto& resid) {
    return existing_resids.find(resid) == existing_resids.end();
  });
  report_metric(OPTRES, "num_ids_from_code", ids_from_code.size(), mgr);

  // 3. Get all resources directly referenced by root XML files (AndroidManifest
  // and anims XML's). These will form the 'base' externally referenced
  // resources.

  // Set of ID's directly accessible from the manifest or anims XML files,
  // without walking any reference chains.
  std::unordered_set<std::string> explored_xml_files;
  std::unordered_set<uint32_t> external_id_roots;
  const auto& xml_files = resources->find_all_xml_files();
  for (const std::string& path : xml_files) {
    if (path.find("AndroidManifest.xml") == std::string::npos &&
        path.find("/res/anim/") == std::string::npos) {
      continue;
    }
    explored_xml_files.emplace(path);
    const auto& id_roots = resources->get_xml_reference_attributes(path);
    external_id_roots.insert(id_roots.begin(), id_roots.end());
  }
  TRACE(OPTRES, 2, "Total external_id_roots count: %zu",
        external_id_roots.size());

  // 4. Get all resources referenced by custom frameworks.
  std::unordered_set<uint32_t> accessible_id_roots;
  auto& plugin_registery = opt_res::ReachableResourcesPluginRegistry::get();
  for (const auto& p : plugin_registery.get_plugins()) {
    auto ids = p->get_reachable_resources(resources->get_base_assets_dir(),
                                          res_table->name_to_ids);
    TRACE(OPTRES, 2, "Plugin %s retaining %zu root(s)", p->get_name().c_str(),
          ids.size());
    accessible_id_roots.insert(ids.begin(), ids.end());
  }

  std::unordered_set<uint32_t> assumed_reachable_roots =
      get_resources_by_name_prefix(m_assume_reachable_prefixes,
                                   res_table->name_to_ids);
  TRACE(OPTRES, 2, "Total assumed_reachable_roots count: %zu",
        assumed_reachable_roots.size());

  // 5a. Merge above resources (2, 3 & 4). These will be the 'roots' of all
  // referenced resources. Then, compute the transitive closure of all the
  // roots. This will be the set of all referenced resources (to be kept).
  accessible_id_roots.insert(external_id_roots.begin(),
                             external_id_roots.end());
  accessible_id_roots.insert(ids_from_code.begin(), ids_from_code.end());
  accessible_id_roots.insert(assumed_reachable_roots.begin(),
                             assumed_reachable_roots.end());

  TRACE(OPTRES, 2, "Root resource count: %zu", accessible_id_roots.size());

  std::unordered_set<uint32_t> nodes_visited;
  compute_transitive_closure(res_table.get(), zip_dir, resources.get(),
                             accessible_id_roots, &nodes_visited,
                             &explored_xml_files);

  // 5b. "Visit" all resources for any disallowed types. This will prevent any
  // cleanup within the disallowed types.
  std::unordered_set<uint32_t> disallowed_types =
      res_table->get_types_by_name(m_disallowed_types);

  std::unordered_set<uint32_t> disallowed_resouces =
      get_disallowed_resources(res_table->sorted_res_ids, disallowed_types);

  nodes_visited.insert(disallowed_resouces.begin(), disallowed_resouces.end());

  // 6. Remove any unvisited resources. The removal of the unused
  //    files happens in step 11 (if configured) and cleanup of unused strings
  //    will happen from main.cpp (if configured by global options).
  std::unordered_set<std::string> files_to_delete;
  std::unordered_set<uint32_t> deleted_resources = delete_unvisited_resources(
      res_table->id_to_name, nodes_visited, res_table.get(), &files_to_delete);

  report_metric(OPTRES, "num_deleted_resources", deleted_resources.size(), mgr);

  if (!m_assume_id_inlined) {
    // 7. Create mapping from kept to remapped resource ID's
    std::map<uint32_t, uint32_t> kept_to_remapped_ids =
        build_remapping(res_table->sorted_res_ids,
                        res_table->id_to_name,
                        deleted_resources,
                        conf.metafile("redex-resid-optres-mapping.json"));

    // 8. Renumber resources in R$ classes and explored_xml_files
    remap_resource_classes(stores, kept_to_remapped_ids);

    for (const std::string& path : explored_xml_files) {
      resources->remap_xml_reference_attributes(path, kept_to_remapped_ids);
    }

    // 9. Fix up the arrays in the base R class, as well as R$styleable- any
    //    deleted entries are removed, the rest are remapped.
    remap_resource_class_arrays(stores, conf.get_global_config(),
                                kept_to_remapped_ids);

    // 10. Renumber all resource referencesand write out the new resource file
    // to disk.
    const auto& res_files = resources->find_resources_files();
    res_table->remap_res_ids_and_serialize(res_files, kept_to_remapped_ids);
  } else {
    // Instead of remapping resource IDs, we nullify resource entries for
    // to_delete resources. This is designed for situations where resource IDs
    // might be inlined before this pass run.
    std::map<uint32_t, uint32_t> kept_ids_to_itself;
    for (size_t index = 0; index < res_table->sorted_res_ids.size(); ++index) {
      uint32_t id = res_table->sorted_res_ids[index];
      if (deleted_resources.find(id) == deleted_resources.end()) {
        kept_ids_to_itself[id] = id;
      }
    }
    remap_resource_class_arrays(stores, conf.get_global_config(),
                                kept_ids_to_itself);
    const auto& res_files = resources->find_resources_files();
    res_table->nullify_res_ids_and_serialize(res_files);
  }

  // 11. If configured, actually remove the resource files we have determined
  // to be unused. This may influence reachability of classes in XML layouts.
  if (m_delete_unused_files) {
    auto deleted = delete_files_relative(zip_dir, files_to_delete);
    report_metric(OPTRES, "num_deleted_files", deleted, mgr);
  }
}

static OptimizeResourcesPass s_pass;
