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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "androidfw/ResourceTypes.h"
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
      const std::unordered_map<std::string, std::string>& old_to_new) override;
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
  std::unordered_set<uint32_t> get_types_by_name(
      const std::unordered_set<std::string>& type_names) override;
  std::unordered_set<uint32_t> get_types_by_name_prefixes(
      const std::unordered_set<std::string>& type_name_prefixes) override;
  std::vector<std::string> get_files_by_rid(
      uint32_t res_id,
      ResourcePathType path_type = ResourcePathType::DevicePath) override;
  void walk_references_for_resource(
      uint32_t resID,
      ResourcePathType path_type,
      std::unordered_set<uint32_t>* nodes_visited,
      std::unordered_set<std::string>* potential_file_paths) override;
  uint64_t resource_value_count(uint32_t res_id) override;
  void delete_resource(uint32_t res_id) override;
  void collect_resource_data_for_file(const std::string& resources_pb_path);
  size_t get_hash_from_values(const ConfigValues& config_values);
  size_t obfuscate_resource_and_serialize(
      const std::vector<std::string>& resource_files,
      const std::map<std::string, std::string>& filepath_old_to_new,
      const std::unordered_set<uint32_t>& allowed_types,
      const std::unordered_set<std::string>& keep_resource_prefixes) override;
  void get_configurations(
      uint32_t package_id,
      const std::string& name,
      std::vector<android::ResTable_config>* configs) override;
  std::set<android::ResTable_config> get_configs_with_values(
      uint32_t id) override;

  const std::map<uint32_t, const ConfigValues>& get_res_id_to_configvalue()
      const {
    return m_res_id_to_configvalue;
  }
  std::string resolve_module_name_for_resource_id(uint32_t res_id);
  std::string resolve_module_name_for_package_id(uint32_t package_id);

 private:
  std::map<uint32_t, std::string> m_type_id_to_names;
  std::unordered_set<uint32_t> m_existed_res_ids;
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
  std::unordered_set<uint32_t> get_xml_reference_attributes(
      const std::string& filename) override;
  void collect_layout_classes_and_attributes_for_file(
      const std::string& file_path,
      const std::unordered_set<std::string>& attributes_to_read,
      std::unordered_set<std::string>* out_classes,
      std::unordered_multimap<std::string, std::string>* out_attributes)
      override;
  size_t remap_xml_reference_attributes(
      const std::string& filename,
      const std::map<uint32_t, uint32_t>& kept_to_remapped_ids) override;
  std::vector<std::string> find_resources_files() override;
  std::unique_ptr<ResourceTableFile> load_res_table() override;

  std::unordered_set<std::string> find_all_xml_files() override;

  std::string get_base_assets_dir() override;

 protected:
  std::vector<std::string> find_res_directories() override;
  std::vector<std::string> find_lib_directories() override;

  bool rename_classes_in_layout(
      const std::string& file_path,
      const std::map<std::string, std::string>& rename_map,
      size_t* out_num_renamed) override;
};

#endif // HAS_PROTOBUF
