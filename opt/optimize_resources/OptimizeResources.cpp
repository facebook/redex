/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OptimizeResources.h"

#include <boost/format.hpp>
#include <json/value.h>
#include <utility>

#include "ConfigFiles.h"
#include "GlobalConfig.h"
#include "IOUtil.h"
#include "PassManager.h"
#include "RClass.h"
#include "RedexResources.h"
#include "Show.h"

namespace opt_res {
ReachableResourcesPlugin::ReachableResourcesPlugin(std::string name)
    : m_name(std::move(name)) {
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
/*
 * Delete unvisited res ids from res table.
 */
UnorderedSet<uint32_t> delete_unvisited_resources(
    const std::string& out_file,
    const std::map<uint32_t, std::string>& id_to_name,
    const std::vector<std::string>& all_types,
    const UnorderedSet<uint32_t>& nodes_visited,
    ResourceTableFile* table,
    UnorderedSet<std::string>* out_files_to_delete) {
  std::fstream out(out_file, std::ios_base::app);
  bool write_to_file = true;
  if (!out.is_open()) {
    fprintf(stderr, "Unable to write the removed symbols into file %s\n",
            out_file.c_str());
    write_to_file = false;
  } else {
    TRACE(OPTRES, 1, "Writing removed resources to %s", out_file.c_str());
  }
  UnorderedSet<uint32_t> deleted_resources;
  UnorderedSet<std::string> files_to_keep;
  for (const auto& p : id_to_name) {
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
  unordered_erase_if(*out_files_to_delete, [&](const auto& filename) {
    return files_to_keep.find(filename) != files_to_keep.end();
  });
  return deleted_resources;
}

std::map<uint32_t, uint32_t> build_remapping(
    const std::vector<uint32_t>& sorted_res_ids,
    const std::map<uint32_t, std::string>& id_to_name,
    const UnorderedSet<uint32_t>& deleted_resources,
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
  for (const auto& p : plugin_registery.get_plugins()) {
    p->configure(conf);
  }
}

void OptimizeResourcesPass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& conf,
                                     PassManager& mgr) {
  std::string zip_dir;
  conf.get_json_config().get("apk_dir", "", zip_dir);
  always_assert(!zip_dir.empty());

  // 1. Load the resource table
  resources::ReachableResources reachable_resources(
      zip_dir, conf.get_global_config(), m_options);
  auto* resources = reachable_resources.get_android_resources();
  auto* res_table = reachable_resources.get_res_table();

  // 2. Read entry points from the app code and manifest to resource ids.
  auto initial_reachable_ids = reachable_resources.get_resource_roots(stores);
  report_metric(OPTRES, "num_ids_from_code",
                reachable_resources.code_roots_size(), mgr);
  TRACE(OPTRES, 2, "Total manifest_roots count: %zu",
        reachable_resources.manifest_roots_size());
  TRACE(OPTRES, 2, "Total assumed_reachable_roots count: %zu",
        reachable_resources.assumed_roots_size());

  // 3. Add additional roots as configured by customizable logic.
  auto& plugin_registery = opt_res::ReachableResourcesPluginRegistry::get();
  for (const auto& p : plugin_registery.get_plugins()) {
    auto ids = p->get_reachable_resources(resources->get_base_assets_dir(),
                                          res_table->name_to_ids);
    TRACE(OPTRES, 2, "Plugin %s retaining %zu root(s)", p->get_name().c_str(),
          ids.size());
    insert_unordered_iterable(initial_reachable_ids, ids);
  }
  TRACE(OPTRES, 2, "Root resource count: %zu", initial_reachable_ids.size());

  // 4. From entry points, compute all things transitively reachable.
  auto nodes_visited =
      reachable_resources.compute_transitive_closure(initial_reachable_ids);
  auto explored_xml_files = reachable_resources.explored_xml_files();
  TRACE(OPTRES, 2, "nodes_visited count: %zu", nodes_visited.size());
  TRACE(OPTRES, 2, "explored_xml_files count: %zu", explored_xml_files.size());

  // 5. Remove any unvisited resources. The removal of the unused
  //    files happens in step 11 (if configured) and cleanup of unused strings
  //    will happen from main.cpp (if configured by global options).
  UnorderedSet<std::string> files_to_delete;
  std::vector<std::string> type_names;
  res_table->get_type_names(&type_names);
  UnorderedSet<uint32_t> deleted_resources = delete_unvisited_resources(
      conf.metafile("redex-removed-resources.txt"), res_table->id_to_name,
      type_names, nodes_visited, res_table, &files_to_delete);
  report_metric(OPTRES, "num_deleted_resources", deleted_resources.size(), mgr);

  resources::RClassWriter r_class_writer(conf.get_global_config());
  if (!m_options.assume_id_inlined) {
    // 6. Create mapping from kept to remapped resource IDs
    std::map<uint32_t, uint32_t> kept_to_remapped_ids =
        build_remapping(res_table->sorted_res_ids,
                        res_table->id_to_name,
                        deleted_resources,
                        conf.metafile("redex-resid-optres-mapping.json"));

    // 7. Renumber resources in R$ classes and explored_xml_files
    r_class_writer.remap_resource_class_scalars(stores, kept_to_remapped_ids);

    for (const std::string& path : UnorderedIterable(explored_xml_files)) {
      resources->remap_xml_reference_attributes(path, kept_to_remapped_ids);
    }

    // 8. Fix up the arrays in the base R class, as well as R$styleable- any
    //    deleted entries are removed, the rest are remapped.
    r_class_writer.remap_resource_class_arrays(stores, kept_to_remapped_ids);

    // 9. Renumber all resource references and write out the new resource table
    // to disk.
    const auto& res_files = resources->find_resources_files();
    res_table->remap_res_ids_and_serialize(res_files, kept_to_remapped_ids);
  } else {
    // Instead of remapping resource IDs, we nullify resource entries for
    // to_delete resources. This is designed for situations where resource IDs
    // might be inlined before this pass runs.
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

  // 10. If configured, actually remove the resource files we have determined
  // to be unused. This may influence reachability of classes in XML layouts.
  if (m_delete_unused_files) {
    auto deleted = delete_files_relative(zip_dir, files_to_delete);
    report_metric(OPTRES, "num_deleted_files", deleted, mgr);
  }
}

static OptimizeResourcesPass s_pass;
