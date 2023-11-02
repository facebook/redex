/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SplitResourceTables.h"

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <cinttypes>
#include <json/value.h>
#include <map>
#include <unordered_set>

#include "ApkResources.h"
#include "ConfigFiles.h"
#include "Creators.h"
#include "DetectBundle.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IOUtil.h"
#include "MethodReference.h"
#include "OptimizeResources.h"
#include "PassManager.h"
#include "RedexResources.h"
#include "Resolver.h"
#include "StaticIds.h"
#include "Trace.h"
#include "Walkers.h"
#include "androidfw/ResourceTypes.h"
#include "utils/Vector.h"

#define RES_GET_IDENTIFIER_SIGNATURE \
  "Landroid/content/res/Resources;"  \
  ".getIdentifier:(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I"

#define RES_GET_TYPENAME_SIGNATURE  \
  "Landroid/content/res/Resources;" \
  ".getResourceTypeName:(I)Ljava/lang/String;"

#define METRIC_ARSC_DELTA "arsc_size_delta"
#define METRIC_RES_CALLS_REWRITTEN "resource_calls_rewritten"
#define METRIC_TYPES_DEFINED "types_defined"
#define METRIC_EMPTY_CELLS_ELIMINATED "empty_cells_eliminated"

#define GET_TYPE(id) (((id) >> TYPE_INDEX_BIT_SHIFT & 0xFF))

namespace {
struct TypeSplit {
  std::string name;
  // 1 based index for the type (must not already exist)
  uint8_t type_idx;
  std::vector<android::ResTable_config> configs;
  // Sorted, list of ids that we should relocate to this new type.
  std::vector<uint32_t> relocate_ids;
  // Used for logging only
  size_t metric_empty_cells_eliminated;
};

void signatures_to_methods(
    const std::unordered_map<std::string, std::string>& signatures,
    std::unordered_map<DexMethod*, DexMethod*>* methods) {
  for (const auto& pair : signatures) {
    auto first = DexMethod::get_method(pair.first);
    always_assert_log(first != nullptr, "Did not find method %s",
                      pair.first.c_str());
    auto second = DexMethod::get_method(pair.second);
    always_assert_log(
        second != nullptr,
        "Method %s does not exist in the app dependencies (or "
        "was deleted). Please ensure this pass is relevant to the app, and if "
        "so edit the config to use a different "
        "wrapper method, or add this method to the app's dependencies.",
        pair.second.c_str());
    auto resolved_second = resolve_method(second, MethodSearch::Static);
    always_assert_log(resolved_second != nullptr,
                      "No static method def found for %s",
                      pair.second.c_str());
    (*methods)[static_cast<DexMethod*>(first)] = resolved_second;
  }
}

std::string configs_to_string(
    const std::set<android::ResTable_config>& configs) {
  std::ostringstream s;
  bool empty = true;
  for (const auto& c : configs) {
    if (!empty) {
      s << ", ";
    }
    empty = false;
    auto desc = c.toString();
    s << (desc.length() > 0 ? desc.string() : "default");
  }
  return s.str();
}

TypeSplit make_split_struct(
    const std::string& base_type_name,
    const size_t splits_created,
    const std::vector<std::string>& type_names,
    const std::set<android::ResTable_config>& config_set,
    const std::set<uint32_t>& ids) {
  std::string new_type_name =
      base_type_name + "." + std::to_string(splits_created + 1);
  auto idx = (uint8_t)(type_names.size() + 1); // type index is 1 based
  TRACE(SPLIT_RES, 2, "Defining new type %s. Identifier: %x, Num IDs: %zu",
        new_type_name.c_str(), idx, ids.size());
  TypeSplit split = {new_type_name, idx, {}, {}, 0};
  for (const auto& c : config_set) {
    split.configs.emplace_back(c);
  }
  for (const auto& i : ids) {
    split.relocate_ids.push_back(i);
  }
  return split;
}

struct ConfigSetStats {
  size_t dead_space;
  std::set<uint32_t> ids_with_values;
};

struct SplitCandidate {
  std::set<android::ResTable_config> config_set;
  ConfigSetStats stats;

  // Desending sort, as we want the highest amount of dead space to be processed
  // first.
  bool operator<(const SplitCandidate& other) const {
    return stats.dead_space > other.stats.dead_space;
  }
};

uint32_t max_id(const std::vector<uint32_t>& sorted_res_ids, uint8_t type_id) {
  uint32_t max = 0;
  for (const auto& id : sorted_res_ids) {
    if (GET_TYPE(id) > type_id) {
      break;
    }
    max = id;
  }
  return max;
}

// Given a type id, figure out if a substantial number of res ids from that type
// contain only values in a subset of configs. If so, emit potentially many
// TypeSplit instances representing res ids to relocate, and new ResTable_type
// structs to create in the arsc file.
// Returns number of splits added to the output vec.
size_t maybe_split_type(
    ResourceTableFile& res_table,
    const std::map<uint8_t, uint32_t>& type_to_movable_entries,
    const uint8_t type_id,
    const std::vector<android::ResTable_config>& all_configs,
    const size_t split_threshold,
    const size_t max_splits,
    std::vector<std::string>* type_names,
    std::vector<TypeSplit>* accumulator) {
  size_t all_configs_size = all_configs.size();
  if (all_configs_size < 2) {
    return 0;
  }
  if (type_to_movable_entries.find(type_id) == type_to_movable_entries.end()) {
    // Type may be entirely filled with fixed ids, don't consider further.
    return 0;
  }

  // For every ID, get the set of configurations which have non-empty values. If
  // that config set "C" is a proper subset of configs in the type, add the
  // amount of dead space the ID will contribute to a runny tally of the dead
  // space created by "C" among other IDs.
  std::map<std::set<android::ResTable_config>, ConfigSetStats> stats;
  for (uint32_t id = type_to_movable_entries.at(type_id);
       id <= max_id(res_table.sorted_res_ids, type_id);
       id++) {
    auto config_set = res_table.get_configs_with_values(id);
    if (!config_set.empty() && config_set.size() < all_configs_size) {
      stats[config_set].dead_space += all_configs_size - config_set.size();
      stats[config_set].ids_with_values.emplace(id);
    }
  }
  if (stats.empty()) {
    return 0;
  }

  // Take the top N most impactful config sets to make a split from.
  std::vector<SplitCandidate> candidates;
  for (const auto& pair : stats) {
    SplitCandidate candidate{pair.first, pair.second};
    candidates.emplace_back(std::move(candidate));
  }
  std::sort(candidates.begin(), candidates.end());

  size_t splits_created = 0;
  auto type_name = type_names->at(type_id - 1);
  for (size_t i = 0; i < candidates.size() && splits_created < max_splits;
       i++) {
    auto& candidate = candidates[i];
    auto id_count = candidate.stats.ids_with_values.size();
    if (id_count >= split_threshold) {
      splits_created++;
      auto config_set_str = configs_to_string(candidate.config_set);
      TRACE(SPLIT_RES, 2, "Type %s, %zu movable values in columns (%s)",
            type_name.c_str(), id_count, config_set_str.c_str());
      auto split = make_split_struct(type_name, splits_created, *type_names,
                                     candidate.config_set,
                                     candidate.stats.ids_with_values);
      split.metric_empty_cells_eliminated =
          id_count * (all_configs_size - candidate.config_set.size());
      type_names->push_back(split.name);
      accumulator->push_back(split);
    }
  }
  return splits_created;
}

void compact_resource_ids(
    const std::vector<uint32_t>& sorted_res_ids,
    const std::map<uint8_t, uint32_t>& type_to_movable_entries,
    const std::unordered_set<uint32_t>& deleted_resources,
    std::map<uint32_t, uint32_t>* old_to_remapped_ids) {
  auto keep_id = [&](uint32_t id) {
    // Ensure that ids that don't get reassigned get considered for having their
    // values remapped. Remapping APIs have wonky conventions where not present
    // can signal deletion.
    always_assert(old_to_remapped_ids->count(id) == 0);
    TRACE(SPLIT_RES, 4, "Keeping id %x", id);
    old_to_remapped_ids->emplace(id, id);
  };
  uint32_t current_type_id = 0;
  size_t current_type_count = 0;
  size_t size = sorted_res_ids.size();
  for (size_t i = 0; i < size; i++) {
    uint32_t id = sorted_res_ids[i];
    uint8_t type_id = GET_TYPE(id);
    if (type_to_movable_entries.find(type_id) ==
        type_to_movable_entries.end()) {
      // Cannot compact this type.
      keep_id(id);
      continue;
    }
    if (type_id != current_type_id) {
      current_type_id = type_id;
      current_type_count = type_to_movable_entries.at(type_id);
    }
    if (id < current_type_count) {
      // Unmovable id, keep it.
      keep_id(id);
      continue;
    }
    if (deleted_resources.count(id) == 0) {
      uint32_t new_id = current_type_count++;
      if (new_id != id) {
        TRACE(SPLIT_RES, 4, "Compacting %x to %x", id, new_id);
        old_to_remapped_ids->emplace(id, new_id);
      } else {
        keep_id(id);
      }
    }
  }
}

void write_remapping_file(
    std::map<uint32_t, std::string>& id_to_name,
    const std::map<uint32_t, uint32_t>& old_to_remapped_ids,
    const std::string& out_file) {
  Json::Value map_json;
  for (const auto& i : old_to_remapped_ids) {
    Json::Value row;
    row["name"] = id_to_name[i.first];
    row["old_id"] = (boost::format("%x") % i.first).str();
    row["new_id"] = (boost::format("%x") % i.second).str();
    map_json.append(row);
  }
  write_string_to_file(out_file, map_json.toStyledString());
}

void dump_metrics(PassManager& mgr) {
  TRACE(SPLIT_RES, 1,
        "Typed defined: %" PRId64
        "\n"
        "Empty cells eliminated: %" PRId64
        "\n"
        "android.content.res.Resources calls rewritten: %" PRId64
        "\n"
        ".arsc size delta (bytes): %" PRId64,
        mgr.get_metric(METRIC_TYPES_DEFINED),
        mgr.get_metric(METRIC_EMPTY_CELLS_ELIMINATED),
        mgr.get_metric(METRIC_RES_CALLS_REWRITTEN),
        mgr.get_metric(METRIC_ARSC_DELTA));
}

std::map<uint8_t, uint32_t> build_movable_id_ranges(
    ResourceTableFile& res_table,
    const std::string& our_package_name,
    const std::string& static_ids_file_path) {
  std::unordered_map<uint8_t, uint32_t> type_to_max_static_id;
  auto callback = [&](const std::string& package_name,
                      const std::string& /* unused */,
                      const std::string& /* unused */,
                      uint32_t id) {
    if (package_name != our_package_name) {
      return;
    }
    uint8_t type = GET_TYPE(id);
    auto prev = type_to_max_static_id[type];
    if (id > prev) {
      type_to_max_static_id[type] = id;
    }
  };
  resources::read_static_ids_file(static_ids_file_path, callback);
  std::map<uint8_t, uint32_t> result;
  for (auto id : res_table.sorted_res_ids) {
    uint8_t type = GET_TYPE(id);
    if (result.count(type) == 0 && id > type_to_max_static_id[type]) {
      result.emplace(type, id);
    }
  }
  return result;
}

// Gets the file size of resources.arsc, or 0 if it does not exist. Metrics for
// tracking size of this file will be unsupported for .aab inputs.
size_t get_arsc_file_size(const std::string& unpack_dir) {
  std::string arsc_path = unpack_dir + std::string("/resources.arsc");
  if (!boost::filesystem::exists(arsc_path)) {
    return 0;
  }
  return boost::filesystem::file_size(arsc_path);
}
} // namespace

bool SplitResourceTablesPass::is_type_allowed(const std::string& type_name) {
  if (m_allowed_types.size() == 1 && *m_allowed_types.begin() == "*") {
    // magic token to enable all types
    return true;
  }
  return m_allowed_types.find(type_name) != m_allowed_types.end();
}

void SplitResourceTablesPass::run_pass(DexStoresVector& stores,
                                       ConfigFiles& cfg,
                                       PassManager& mgr) {
  std::string zip_dir;
  cfg.get_json_config().get("apk_dir", "", zip_dir);
  always_assert(zip_dir.size());

  TRACE(SPLIT_RES, 2, "Begin SplitResourceTablesPass");
  auto resources = create_resource_reader(zip_dir);
  auto res_table = resources->load_res_table();
  auto initial_arsc_length = get_arsc_file_size(zip_dir);

  // An assumption made throughout the rest of the optimization, bail early if
  // this is not accurate.
  always_assert(res_table->package_count() == 1);
  uint32_t package_id = PACKAGE_RESID_START >> PACKAGE_INDEX_BIT_SHIFT;

  // Among the allowed types from the config file, which ones have multiple
  // configurations?
  std::map<size_t, std::vector<android::ResTable_config>> type_to_configs;
  std::vector<std::string> type_names;
  res_table->get_type_names(&type_names);
  for (size_t i = 0; i < type_names.size(); ++i) {
    auto name = type_names[i];
    if (!is_type_allowed(name)) {
      continue;
    }
    std::vector<android::ResTable_config> configs;
    res_table->get_configurations(package_id, name, &configs);
    auto cfg_size = configs.size();
    if (cfg_size > 1) {
      TRACE(SPLIT_RES, 2, "Type %s has %zu configurations", name.c_str(),
            cfg_size);
      for (size_t j = 0; j < cfg_size; j++) {
        auto c = configs[j];
        auto cname = c.toString();
        auto cfg_name = cname.size() == 0 ? "(default)" : cname.string();
        TRACE(SPLIT_RES, 3, "Type %s, config name: %s", name.c_str(), cfg_name);
        type_to_configs[i + 1].push_back(configs[j]);
      }
    }
  }

  // For each resource type, find the smallest entry number that has a name that
  // isn't marked as having a fixed ID in the given .txt file. Entries greater
  // than or equal to this entry can be moved (keep in mind that there could be
  // zero such entries).
  // This should cover the case where the input has holes or even all empty
  // items in the beginning to force the numbering constraints for static ids.
  // Key in here is the right shifted type id, i.e. "1" for 0x7f01xxxx.
  auto package_name = resources->get_manifest_package_name();
  std::map<uint8_t, uint32_t> type_to_movable_entries = build_movable_id_ranges(
      *res_table, *package_name, m_static_ids_file_path);

  // Gather any resource ids that would benefit from splitting.
  std::vector<TypeSplit> new_types;
  for (const auto& i : type_to_configs) {
    maybe_split_type(*res_table,
                     type_to_movable_entries,
                     i.first,
                     i.second,
                     m_split_threshold,
                     m_max_splits_per_type,
                     &type_names,
                     &new_types);
  }

  mgr.incr_metric(METRIC_TYPES_DEFINED, new_types.size());

  // Relocating ids to a new type requires appending to the ResStringPool of
  // type names, and defining a new ResTable_typeSpec and ResTable_type.
  std::map<uint32_t, uint32_t> old_to_remapped_ids;
  std::unordered_set<uint32_t> deleted_resources;
  for (const auto& t : new_types) {
    auto num_ids = t.relocate_ids.size();
    mgr.incr_metric(METRIC_EMPTY_CELLS_ELIMINATED,
                    t.metric_empty_cells_eliminated);

    for (size_t i = 0; i < num_ids; i++) {
      uint32_t old_id = t.relocate_ids[i];
      uint32_t new_id =
          PACKAGE_RESID_START |
          static_cast<uint8_t>(t.type_idx) << TYPE_INDEX_BIT_SHIFT | i;
      old_to_remapped_ids.emplace(old_id, new_id);
      deleted_resources.emplace(old_id);
      TRACE(SPLIT_RES, 4, "Remapping %x to %x", old_id, new_id);
    }
  }
  // Mark old ids as removed in the resource table. This has the side effect of
  // compacting the remaining ids, which means those in turn must be remapped.
  for (const auto& id : deleted_resources) {
    res_table->delete_resource(id);
  }

  // Compute any changed ids as a result of deletion.
  compact_resource_ids(res_table->sorted_res_ids, type_to_movable_entries,
                       deleted_resources, &old_to_remapped_ids);

  // Renumber the R classes
  OptimizeResourcesPass::remap_resource_classes(stores, old_to_remapped_ids);

  // Fix xml files
  auto all_xml_files = resources->find_all_xml_files();
  for (const auto& f : all_xml_files) {
    TRACE(SPLIT_RES, 4, "Remapping XML: %s", f.c_str());
    resources->remap_xml_reference_attributes(f, old_to_remapped_ids);
  }

  OptimizeResourcesPass::remap_resource_class_arrays(
      stores, cfg.get_global_config(), old_to_remapped_ids);

  // Set up the new types that will actually be created by the next step.
  for (const auto& t : new_types) {
    std::vector<android::ResTable_config*> config_ptrs;
    config_ptrs.reserve(t.configs.size());
    for (auto& config : t.configs) {
      config_ptrs.emplace_back(const_cast<android::ResTable_config*>(&config));
    }
    res_table->define_type(package_id, t.type_idx, t.name, config_ptrs,
                           t.relocate_ids);
  }

  // Ensure references to relocated IDs get handled properly. This
  // implementation will also apply any pending new types that were added.
  const auto& res_files = resources->find_resources_files();
  res_table->remap_res_ids_and_serialize(res_files, old_to_remapped_ids);

  // For .aab inputs this metric will be zero, as it will not directly be
  // meaningful to measure at this point.
  mgr.set_metric(METRIC_ARSC_DELTA,
                 get_arsc_file_size(zip_dir) - initial_arsc_length);

  // Make sure we don't break android.content.res.Resources calls, such as
  // getIdentifier().
  std::unordered_map<std::string, std::string> framework_to_compat_signatures;
  if (!m_getidentifier_compat_method.empty()) {
    framework_to_compat_signatures.emplace(RES_GET_IDENTIFIER_SIGNATURE,
                                           m_getidentifier_compat_method);
  }
  if (!m_typename_compat_method.empty()) {
    framework_to_compat_signatures.emplace(RES_GET_TYPENAME_SIGNATURE,
                                           m_typename_compat_method);
  }
  std::unordered_map<DexMethod*, DexMethod*> methods;
  signatures_to_methods(framework_to_compat_signatures, &methods);
  int replaced =
      method_reference::wrap_instance_call_with_static(stores, methods);
  mgr.set_metric(METRIC_RES_CALLS_REWRITTEN, replaced);

  write_remapping_file(res_table->id_to_name,
                       old_to_remapped_ids,
                       cfg.metafile("redex-resid-splitres-mapping.json"));

  dump_metrics(mgr);
}

static SplitResourceTablesPass s_pass;
