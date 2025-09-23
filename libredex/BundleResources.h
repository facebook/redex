/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// TODO (T91001948): Integrate protobuf dependency in supported platforms for
// open source
#ifdef HAS_PROTOBUF
#include "RedexResources.h"

#include <map>
#include <string>
#include <vector>

#include "DeterministicContainers.h"
#include "GlobalConfig.h"
#include "androidfw/ResourceTypes.h"
#include "protocfg/config.pb.h"
#include "protores/Resources.pb.h"
#include <google/protobuf/repeated_field.h>

using ConfigValues = google::protobuf::RepeatedPtrField<aapt::pb::ConfigValue>;

class ResourcesPbFile : public ResourceTableFile {
 public:
  ~ResourcesPbFile() override;
  size_t package_count() override;
  void collect_resid_values_and_hashes(
      const std::vector<uint32_t>& ids,
      std::map<size_t, std::vector<uint32_t>>* res_by_hash) override;
  void remap_file_paths_and_serialize(
      const std::vector<std::string>& resource_files,
      const UnorderedMap<std::string, std::string>& old_to_new) override;
  void remap_res_ids_and_serialize(
      const std::vector<std::string>& resource_files,
      const std::map<uint32_t, uint32_t>& old_to_new) override;
  void nullify_res_ids_and_serialize(
      const std::vector<std::string>& resource_files) override;
  void remap_reorder_and_serialize(
      const std::vector<std::string>& resource_files,
      const std::map<uint32_t, uint32_t>& old_to_new) override;
  bool resource_value_identical(uint32_t a_id, uint32_t b_id) override;
  void get_type_names(std::vector<std::string>* type_names) override;
  UnorderedSet<uint32_t> get_types_by_name(
      const UnorderedSet<std::string>& type_names) override;
  UnorderedSet<uint32_t> get_types_by_name_prefixes(
      const UnorderedSet<std::string>& type_name_prefixes) override;
  std::vector<std::string> get_files_by_rid(
      uint32_t res_id,
      ResourcePathType path_type = ResourcePathType::DevicePath) override;
  void walk_references_for_resource(
      uint32_t resID,
      const ResourcePathType& path_type,
      const resources::ReachabilityOptions& reachability_options,
      UnorderedSet<uint32_t>* nodes_visited,
      UnorderedSet<std::string>* potential_file_paths) override;
  uint64_t resource_value_count(uint32_t res_id) override;
  void delete_resource(uint32_t res_id) override;
  void collect_resource_data_for_file(const std::string& resources_pb_path);
  size_t get_hash_from_values(const ConfigValues& config_values);
  size_t obfuscate_resource_and_serialize(
      const std::vector<std::string>& resource_files,
      const std::map<std::string, std::string>& filepath_old_to_new,
      const UnorderedSet<uint32_t>& allowed_types,
      const UnorderedSet<std::string>& keep_resource_prefixes,
      const UnorderedSet<std::string>& keep_resource_specific) override;
  void get_configurations(
      uint32_t package_id,
      const std::string& name,
      std::vector<android::ResTable_config>* configs) override;
  std::set<android::ResTable_config> get_configs_with_values(
      uint32_t id) override;
  void resolve_string_values_for_resource_reference(
      uint32_t ref, std::vector<std::string>* values) override;
  UnorderedMap<uint32_t, resources::InlinableValue>
  get_inlinable_resource_values() override;
  UnorderedSet<uint32_t> get_overlayable_id_roots() override;
  resources::StyleMap get_style_map() override;
  void apply_attribute_removals(
      const std::vector<resources::StyleModificationSpec::Modification>&
          modifications,
      const std::vector<std::string>& resources_pb_paths) override;
  void apply_attribute_additions(
      const std::vector<resources::StyleModificationSpec::Modification>&
          modifications,
      const std::vector<std::string>& resources_pb_paths) override;

  const std::map<uint32_t, const ConfigValues>& get_res_id_to_configvalue()
      const {
    return m_res_id_to_configvalue;
  }
  std::string resolve_module_name_for_resource_id(uint32_t res_id);
  std::string resolve_module_name_for_package_id(uint32_t package_id);

 private:
  std::map<uint32_t, std::string> m_type_id_to_names;
  UnorderedSet<uint32_t> m_existed_res_ids;
  std::map<uint32_t, const aapt::pb::Entry> m_res_id_to_entry;
  std::map<uint32_t, const ConfigValues> m_res_id_to_configvalue;
  std::map<uint32_t, std::string> m_package_id_to_module_name;
  std::set<uint32_t> m_package_ids;
};

class BundleResources : public AndroidResources {
 public:
  explicit BundleResources(const std::string& directory)
      : AndroidResources(directory) {}
  ~BundleResources() override;
  boost::optional<int32_t> get_min_sdk() override;
  ManifestClassInfo get_manifest_class_info() override;
  boost::optional<std::string> get_manifest_package_name() override;
  UnorderedSet<std::string> get_service_loader_classes() override;
  UnorderedSet<uint32_t> get_xml_reference_attributes(
      const std::string& filename) override;
  void collect_layout_classes_and_attributes_for_file(
      const std::string& file_path,
      const UnorderedSet<std::string>& attributes_to_read,
      resources::StringOrReferenceSet* out_classes,
      std::unordered_multimap<std::string, resources::StringOrReference>*
          out_attributes) override;
  void collect_xml_attribute_string_values_for_file(
      const std::string& file_path, UnorderedSet<std::string>* out) override;
  void fully_qualify_layout(
      const UnorderedMap<std::string, std::string>& element_to_class_name,
      const std::string& file_path,
      size_t* changes) override;
  size_t remap_xml_reference_attributes(
      const std::string& filename,
      const std::map<uint32_t, uint32_t>& kept_to_remapped_ids) override;
  std::vector<std::string> find_resources_files() override;
  std::unique_ptr<ResourceTableFile> load_res_table() override;

  UnorderedSet<std::string> find_all_xml_files() override;

  std::string get_base_assets_dir() override;

  void obfuscate_xml_files(
      const UnorderedSet<std::string>& allowed_types,
      const UnorderedSet<std::string>& do_not_obfuscate_elements) override;
  void finalize_bundle_config(const ResourceConfig& config) override;

 protected:
  std::vector<std::string> find_res_directories() override;
  std::vector<std::string> find_lib_directories() override;

  bool rename_classes_in_layout(
      const std::string& file_path,
      const std::map<std::string, std::string>& rename_map,
      size_t* out_num_renamed) override;
};

bool does_resource_exists_in_file(uint32_t resource_id,
                                  const std::string& file_path);

#endif // HAS_PROTOBUF
