/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "androidfw/ResourceTypes.h"

#include "RedexMappedFile.h"
#include "RedexResources.h"

// Compiled XML reading helper functions. Only applicable to APK input files.
std::string get_string_attribute_value(const android::ResXMLTree& parser,
                                       const android::String16& attribute_name);

bool has_raw_attribute_value(const android::ResXMLTree& parser,
                             const android::String16& attribute_name,
                             android::Res_value& out_value);

int get_int_attribute_or_default_value(const android::ResXMLTree& parser,
                                       const android::String16& attribute_name,
                                       int32_t default_value);

bool has_bool_attribute(const android::ResXMLTree& parser,
                        const android::String16& attribute_name);

bool get_bool_attribute_value(const android::ResXMLTree& parser,
                              const android::String16& attribute_name,
                              bool default_value);

namespace apk {
std::string get_string_from_pool(const android::ResStringPool& pool,
                                 size_t idx);

struct TypeDefinition {
  uint32_t package_id;
  uint8_t type_id;
  android::String8 name8;
  android::String16 name16;
  std::vector<android::ResTable_config*> configs;
  std::vector<uint32_t> source_res_ids;
};
} // namespace apk

class ResourcesArscFile : public ResourceTableFile {
 public:
  ResourcesArscFile(const ResourcesArscFile&) = delete;
  ResourcesArscFile& operator=(const ResourcesArscFile&) = delete;

  android::ResTable res_table;

  explicit ResourcesArscFile(const std::string& path);
  std::vector<std::string> get_resource_strings_by_name(
      const std::string& res_name);
  void remap_ids(const std::map<uint32_t, uint32_t>& old_to_remapped_ids);
  size_t serialize();

  void collect_resid_values_and_hashes(
      const std::vector<uint32_t>& ids,
      std::map<size_t, std::vector<uint32_t>>* res_by_hash) override;
  bool resource_value_identical(uint32_t a_id, uint32_t b_id) override;
  std::unordered_set<uint32_t> get_types_by_name(
      const std::unordered_set<std::string>& type_names) override;
  void delete_resource(uint32_t res_id) override;
  void remap_res_ids_and_serialize(
      const std::vector<std::string>& resource_files,
      const std::map<uint32_t, uint32_t>& old_to_new) override;
  void remap_file_paths_and_serialize(
      const std::vector<std::string>& resource_files,
      const std::unordered_map<std::string, std::string>& old_to_new) override;
  void remove_unreferenced_strings() override;
  std::vector<std::string> get_files_by_rid(
      uint32_t res_id,
      ResourcePathType path_type = ResourcePathType::DevicePath) override;
  void walk_references_for_resource(
      uint32_t resID,
      std::unordered_set<uint32_t>* nodes_visited,
      std::unordered_set<std::string>* potential_file_paths) override;
  // Takes effect during serialization, in which new type spec, type data
  // structures will be appended to the package, with entry/value data copied
  // from the given ids. Actual type data in the resulting file will be emitted
  // in the order as the given configs.
  void define_type(uint32_t package_id,
                   uint8_t type_id,
                   const std::string& name,
                   const std::vector<android::ResTable_config*>& configs,
                   const std::vector<uint32_t>& source_res_ids) {
    LOG_ALWAYS_FATAL_IF((package_id & 0xFFFFFF00) != 0,
                        "package_id expected to have low byte set; got 0x%x",
                        package_id);
    android::String8 name8(name.data());
    android::String16 name16(name8);
    apk::TypeDefinition def{package_id, type_id, name8,
                            name16,     configs, source_res_ids};
    m_added_types.emplace_back(std::move(def));
  }
  ~ResourcesArscFile() override;

  size_t get_length() const;

 private:
  std::string m_path;
  RedexMappedFile m_f;
  size_t m_arsc_len;
  std::map<uint32_t, android::Vector<android::Res_value>> tmp_id_to_values;
  bool m_file_closed = false;
  std::unordered_set<uint32_t> m_ids_to_remove;
  std::vector<apk::TypeDefinition> m_added_types;
};

class ApkResources : public AndroidResources {
 public:
  explicit ApkResources(const std::string& directory)
      : AndroidResources(directory),
        m_manifest(directory + "/AndroidManifest.xml") {}
  ~ApkResources() override;

  boost::optional<int32_t> get_min_sdk() override;
  ManifestClassInfo get_manifest_class_info() override;
  std::unordered_set<uint32_t> get_xml_reference_attributes(
      const std::string& filename) override;

  void collect_layout_classes_and_attributes_for_file(
      const std::string& file_path,
      const std::unordered_set<std::string>& attributes_to_read,
      std::unordered_set<std::string>* out_classes,
      std::unordered_multimap<std::string, std::string>* out_attributes)
      override;

  // Given the bytes of a binary XML file, replace the entries (if any) in the
  // ResStringPool. Writes result to the given Vector output param.
  // Returns android::NO_ERROR (0) on success, or one of the corresponding
  // android:: error codes for failure conditions/bad input data.
  int replace_in_xml_string_pool(
      const void* data,
      const size_t len,
      const std::map<std::string, std::string>& rename_map,
      android::Vector<char>* out_data,
      size_t* out_num_renamed);

  size_t remap_xml_reference_attributes(
      const std::string& filename,
      const std::map<uint32_t, uint32_t>& kept_to_remapped_ids) override;
  std::unique_ptr<ResourceTableFile> load_res_table() override;
  std::unordered_set<std::string> find_all_xml_files() override;
  std::vector<std::string> find_resources_files() override;
  std::string get_base_assets_dir() override;

 protected:
  std::vector<std::string> find_res_directories() override;
  std::vector<std::string> find_lib_directories() override;

  // Replaces all strings in the ResStringPool for the given file with their
  // replacements. Writes all changes to disk, clobbering the given file.
  bool rename_classes_in_layout(
      const std::string& file_path,
      const std::map<std::string, std::string>& rename_map,
      size_t* out_num_renamed) override;

 private:
  const std::string m_manifest;
};
