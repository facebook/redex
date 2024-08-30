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
#include "PassManager.h"
#include "RClass.h"
#include "ReachableClasses.h"
#include "RedexMappedFile.h"
#include "RedexResources.h"
#include "Resolver.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "StlUtil.h"
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
        auto r_str = std::string(zip_dir).append("/").append(str);
        if (explored_xml_files->find(r_str) == explored_xml_files->end()) {
          next_xml_files.emplace(std::move(r_str));
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
    const std::string& out_file,
    const std::map<uint32_t, std::string>& id_to_name,
    const std::vector<std::string>& all_types,
    const std::unordered_set<uint32_t>& nodes_visited,
    ResourceTableFile* table,
    std::unordered_set<std::string>* out_files_to_delete) {
  std::fstream out(out_file, std::ios_base::app);
  bool write_to_file = true;
  if (!out.is_open()) {
    fprintf(stderr, "Unable to write the removed symbols into file %s\n",
            out_file.c_str());
    write_to_file = false;
  } else {
    TRACE(OPTRES, 1, "Writing removed resources to %s", out_file.c_str());
  }
  std::unordered_set<uint32_t> deleted_resources;
  std::unordered_set<std::string> files_to_keep;
  for (auto& p : id_to_name) {
    if (nodes_visited.find(p.first) == nodes_visited.end()) {
      // Collect any res/ files we can now delete. This will influence
      // reachability of Java classes. When handling an .aab input, resolve the
      // on-device file paths to their path relative to unpack dir.
      if (write_to_file) {
        out << all_types.at(
                   ((p.first & TYPE_MASK_BIT) >> TYPE_INDEX_BIT_SHIFT) - 1)
            << "/" << p.second << std::endl;
      }
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

std::unordered_set<uint32_t> find_code_resource_references(
    DexStoresVector& stores,
    const resources::RClassReader& r_class_reader,
    const std::map<std::string, std::vector<uint32_t>>& name_to_ids,
    bool check_string_for_name,
    bool assume_id_inlined) {
  std::unordered_set<uint32_t> ids_from_code;
  Scope scope = build_class_scope(stores);
  ConcurrentSet<uint32_t> potential_ids_from_code;
  ConcurrentSet<DexField*> accessed_sfields;
  ConcurrentSet<uint32_t> potential_ids_from_strings;
  boost::regex find_ints("(\\d+)");

  walk::parallel::opcodes(scope, [&](DexMethod* m, IRInstruction* insn) {
    // Collect all accessed fields that could be R fields, or values that got
    // inlined elsewhere.
    if (insn->has_field() && opcode::is_an_sfield_op(insn->opcode())) {
      auto field = resolve_field(insn->get_field(), FieldSearch::Static);
      if (field && field->is_concrete()) {
        accessed_sfields.emplace(field);
      }
    } else if (insn->has_literal()) {
      auto lit = insn->get_literal();
      if (assume_id_inlined && resources::is_potential_resid(lit)) {
        potential_ids_from_code.emplace(lit);
      }
    } else if (insn->has_string()) {
      std::string to_find = insn->get_string()->str_copy();
      if (assume_id_inlined) {
        // Redex evaluates expressions like
        // String.valueOf(R.drawable.inspiration_no_format)
        // which means we need to parse ints encoded as strings or ints that
        // were constant folded/concatenated at build time with other strings.
        std::vector<std::string> int_strings;
        int_strings.insert(int_strings.end(),
                           boost::sregex_token_iterator(
                               to_find.begin(), to_find.end(), find_ints),
                           boost::sregex_token_iterator());
        for (const auto& int_string : int_strings) {
          int64_t potential_num;
          try {
            potential_num = std::stol(int_string);
          } catch (...) {
            continue;
          }
          if (resources::is_potential_resid(potential_num)) {
            potential_ids_from_code.emplace(potential_num);
          }
        }
      }
      if (check_string_for_name) {
        // Being more conservative of what might get passed into
        // Landroid/content/res/Resources;.getIdentifier:(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I
        auto search = name_to_ids.find(to_find);
        if (search != name_to_ids.end()) {
          potential_ids_from_strings.insert(search->second.begin(),
                                            search->second.end());
        }
      }
    } else if (assume_id_inlined && insn->opcode() == OPCODE_FILL_ARRAY_DATA) {
      auto op_data = insn->get_data();
      auto cls = type_class(m->get_class());
      // Do not blanket assume the filling of customized arrays is a usage.
      auto customized_r = !resources::is_non_customized_r_class(cls) &&
                          r_class_reader.is_r_class(cls);
      if (!customized_r && fill_array_data_payload_width(op_data) == 4) {
        // Consider only int[] for resource ids.
        auto payload = get_fill_array_data_payload<uint32_t>(op_data);
        for (const auto& lit : payload) {
          if (resources::is_potential_resid(lit)) {
            potential_ids_from_code.emplace(lit);
          }
        }
      }
    }
  });

  std::unordered_set<DexField*> array_fields;
  for (auto* field : accessed_sfields) {
    auto is_r_field =
        resources::is_non_customized_r_class(type_class(field->get_class()));
    if (type::is_primitive(field->get_type()) && field->get_static_value() &&
        resources::is_potential_resid(field->get_static_value()->value()) &&
        (is_r_field || assume_id_inlined)) {
      ids_from_code.emplace(field->get_static_value()->value());
    } else if (is_r_field && type::is_array(field->get_type())) {
      array_fields.emplace(field);
    }
  }

  r_class_reader.extract_resource_ids_from_static_arrays(scope, array_fields,
                                                         &ids_from_code);
  ids_from_code.insert(potential_ids_from_code.begin(),
                       potential_ids_from_code.end());
  ids_from_code.insert(potential_ids_from_strings.begin(),
                       potential_ids_from_strings.end());
  return ids_from_code;
}
} // namespace

void OptimizeResourcesPass::report_metric(TraceModule trace_module,
                                          const std::string& metric_name,
                                          int metric_value,
                                          PassManager& mgr) {
  TRACE(trace_module, 1, "%s: %d", metric_name.c_str(), metric_value);
  mgr.set_metric(metric_name, metric_value);
}

void OptimizeResourcesPass::eval_pass(DexStoresVector& stores,
                                      ConfigFiles& conf,
                                      PassManager&) {
  resources::prepare_r_classes(stores, conf.get_global_config());
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
  always_assert(!zip_dir.empty());

  // 1. Get all known resource ID's from either resources.pb(AAB) or
  // resources.arsc(APK) file.
  auto resources = create_resource_reader(zip_dir);
  auto res_table = resources->load_res_table();

  // 2. Get all resources directly referenced by source code.
  resources::RClassReader r_class_reader(conf.get_global_config());
  std::unordered_set<uint32_t> ids_from_code = find_code_resource_references(
      stores, r_class_reader, res_table->name_to_ids, m_check_string_for_name,
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
    if (path.find("AndroidManifest.xml") == std::string::npos) {
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
  std::vector<std::string> type_names;
  res_table->get_type_names(&type_names);
  std::unordered_set<uint32_t> deleted_resources = delete_unvisited_resources(
      conf.metafile("redex-removed-resources.txt"), res_table->id_to_name,
      type_names, nodes_visited, res_table.get(), &files_to_delete);
  report_metric(OPTRES, "num_deleted_resources", deleted_resources.size(), mgr);

  resources::RClassWriter r_class_writer(conf.get_global_config());
  if (!m_assume_id_inlined) {
    // 7. Create mapping from kept to remapped resource ID's
    std::map<uint32_t, uint32_t> kept_to_remapped_ids =
        build_remapping(res_table->sorted_res_ids,
                        res_table->id_to_name,
                        deleted_resources,
                        conf.metafile("redex-resid-optres-mapping.json"));

    // 8. Renumber resources in R$ classes and explored_xml_files
    r_class_writer.remap_resource_class_scalars(stores, kept_to_remapped_ids);

    for (const std::string& path : explored_xml_files) {
      resources->remap_xml_reference_attributes(path, kept_to_remapped_ids);
    }

    // 9. Fix up the arrays in the base R class, as well as R$styleable- any
    //    deleted entries are removed, the rest are remapped.
    r_class_writer.remap_resource_class_arrays(stores, kept_to_remapped_ids);

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
    r_class_writer.remap_resource_class_arrays(stores, kept_ids_to_itself);
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
