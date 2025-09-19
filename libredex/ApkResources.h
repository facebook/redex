/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <sys/types.h>
#include <vector>

#include "androidfw/ResourceTypes.h"
#include "utils/Serialize.h"
#include "utils/Visitor.h"

#include "DeterministicContainers.h"
#include "GlobalConfig.h"
#include "RedexMappedFile.h"
#include "RedexResources.h"

// Compiled XML reading helper functions. Only applicable to APK input files.
std::string convert_from_string16(const android::String16& string16);
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
class XmlValueCollector : public arsc::XmlFileVisitor {
 public:
  ~XmlValueCollector() override {}

  bool visit_attribute_ids(android::ResChunk_header* /* unused */,
                           uint32_t* id,
                           size_t count) override {
    for (size_t i = 0; i < count; i++) {
      auto res_id = id[i];
      if (res_id > PACKAGE_RESID_START) {
        m_ids.emplace(res_id);
      }
    }
    return true;
  }

  bool visit_typed_data(android::Res_value* value) override {
    auto data_type = value->dataType;
    auto res_id = dtohl(value->data);
    if (res_id > PACKAGE_RESID_START &&
        (data_type == android::Res_value::TYPE_REFERENCE ||
         data_type == android::Res_value::TYPE_ATTRIBUTE)) {
      m_ids.emplace(res_id);
    }
    return true;
  }

  UnorderedSet<uint32_t> m_ids;
};

class XmlFileEditor : public arsc::XmlFileVisitor {
 public:
  ~XmlFileEditor() override {}

  bool visit_global_strings(android::ResStringPool_header* pool) override;
  bool visit_attribute_ids(android::ResChunk_header* header,
                           uint32_t* id,
                           size_t count) override;
  bool visit_typed_data(android::Res_value* value) override;
  // Remaps attribute IDs and reference data, according to the map and returns
  // the number of changes made.
  size_t remap(const std::map<uint32_t, uint32_t>& old_to_new);

  android::ResStringPool_header* m_string_pool_header = nullptr;
  size_t m_attribute_id_count = 0;
  uint32_t* m_attribute_ids_start = nullptr;
  std::vector<android::ResXMLTree_attribute*> m_attributes;
  std::vector<android::Res_value*> m_typed_data;
};

using OverlayLookup =
    std::map<android::ResTable_overlayable_header*, arsc::OverlayInfo>;

// Read the ResTable data structures and store a convenient organization of the
// data pointers and the packages.
// NOTE: Visitor super classes simply follow pointers, so all subclasses which
// make use of/change data need to be aware of the potential for "canonical"
// data offsets (hence the use of sets).
class TableParser : public arsc::StringPoolRefVisitor {
 public:
  ~TableParser() override {}

  bool visit_global_strings(android::ResStringPool_header* pool) override;
  bool visit_package(android::ResTable_package* package) override;
  bool visit_key_strings(android::ResTable_package* package,
                         android::ResStringPool_header* pool) override;
  bool visit_type_strings(android::ResTable_package* package,
                          android::ResStringPool_header* pool) override;
  bool visit_type_spec(android::ResTable_package* package,
                       android::ResTable_typeSpec* type_spec) override;
  bool visit_type(android::ResTable_package* package,
                  android::ResTable_typeSpec* type_spec,
                  android::ResTable_type* type) override;
  bool visit_overlayable(
      android::ResTable_package* package,
      android::ResTable_overlayable_header* overlayable) override;
  bool visit_overlayable_policy(
      android::ResTable_package* package,
      android::ResTable_overlayable_header* overlayable,
      android::ResTable_overlayable_policy_header* policy,
      uint32_t* ids_ptr) override;
  bool visit_overlayable_id(android::ResTable_package*,
                            android::ResTable_overlayable_header*,
                            android::ResTable_overlayable_policy_header*,
                            uint32_t id) override;
  bool visit_unknown_chunk(android::ResTable_package* package,
                           android::ResChunk_header* header) override;

  android::ResStringPool_header* m_global_pool_header;
  std::map<android::ResTable_package*, android::ResStringPool_header*>
      m_package_key_string_headers;
  std::map<android::ResTable_package*, android::ResStringPool_header*>
      m_package_type_string_headers;
  // Simple organization of each ResTable_typeSpec in the package, and each
  // spec's types/configs.
  std::map<android::ResTable_package*, std::vector<arsc::TypeInfo>>
      m_package_types;
  std::set<android::ResTable_package*> m_packages;
  std::map<android::ResTable_package*, OverlayLookup> m_package_overlayables;
  UnorderedSet<uint32_t> m_overlayable_ids;
  // Chunks belonging to a package that we do not parse/edit. Meant to be
  // preserved as-is when preparing output file.
  std::map<android::ResTable_package*, std::vector<android::ResChunk_header*>>
      m_package_unknown_chunks;
};

using TypeToEntries =
    std::map<android::ResTable_type*, std::vector<arsc::EntryValueData>>;
using ConfigToEntry = std::map<android::ResTable_config*, arsc::EntryValueData>;

class TableEntryParser : public TableParser {
 public:
  ~TableEntryParser() override {}

  bool visit_type_spec(android::ResTable_package* package,
                       android::ResTable_typeSpec* type_spec) override;
  bool visit_type(android::ResTable_package* package,
                  android::ResTable_typeSpec* type_spec,
                  android::ResTable_type* type) override;

  // Convenience methods to look up data about a type in a package
  std::vector<android::ResTable_config*> get_configs(uint32_t package_id,
                                                     uint8_t type_id) {
    std::vector<android::ResTable_config*> vec;
    auto key = make_package_type_id(package_id, type_id);
    if (m_types_to_configs.count(key) > 0) {
      auto& types = m_types_to_configs.at(key);
      vec.reserve(types.size());
      for (auto& t : types) {
        vec.emplace_back(&t->config);
      }
    }
    return vec;
  }

  arsc::EntryValueData get_entry_for_config(uint32_t res_id,
                                            android::ResTable_config* config) {
    auto& config_to_entry = m_res_id_to_entries.at(res_id);
    return config_to_entry.at(config);
  }

  // For a package, the mapping from each type within all type specs to all the
  // entries/values.
  UnorderedMap<android::ResTable_package*, TypeToEntries> m_types_to_entries;
  // Package and type ID to spec
  std::map<uint16_t, android::ResTable_typeSpec*> m_types;
  // Package and type ID to all configs in that type
  std::map<uint16_t, std::vector<android::ResTable_type*>> m_types_to_configs;
  // Resource ID to the corresponding entries (in all configs).
  std::map<uint32_t, ConfigToEntry> m_res_id_to_entries;
  std::map<uint32_t, uint32_t> m_res_id_to_flags;

 private:
  // Convenience function to make it easy to uniquely refer to a type.
  uint16_t make_package_type_id(uint32_t package_id, uint8_t type_id) {
    return ((package_id & 0xFF) << 8) | type_id;
  }
  uint16_t make_package_type_id(android::ResTable_package* package,
                                uint8_t type_id) {
    return make_package_type_id(dtohl(package->id), type_id);
  }
  void put_entry_data(uint32_t res_id,
                      android::ResTable_package* package,
                      android::ResTable_type* type,
                      arsc::EntryValueData& data);
};

// Holds parsed details of the .arsc file. Make sure to disregarded/regenerate
// this when the backing file on disk gets modified.
// NOTE: this class should ideally not leak out into any optimization passes,
// but may be useful directly from test cases.
class TableSnapshot {
 public:
  TableSnapshot(RedexMappedFile&, size_t);
  // Gather all resource identifiers that have some non-empty value in a config.
  void gather_non_empty_resource_ids(std::vector<uint32_t>* ids);
  std::string get_resource_name(uint32_t id);
  // The number of packages in the table.
  size_t package_count();
  // Given a package id (shifted to low bits) emit the values from the type
  // strings pool.
  void get_type_names(uint32_t package_id, std::vector<std::string>* out);
  // Fills the output vec with ResTable_config objects for the given type in the
  // package
  void get_configurations(uint32_t package_id,
                          const std::string& type_name,
                          std::vector<android::ResTable_config>* out);
  // For a given resource ID, return the configs for which the value is nonempty
  std::set<android::ResTable_config> get_configs_with_values(uint32_t id);
  // Returns true if the given ids are from the same type, and all
  // entries/values in all configurations are byte for byte identical.
  bool are_values_identical(uint32_t a, uint32_t b);
  // For every non-empty entry in all configs, coalesce the entry into a list of
  // values. For complex entries, this emits Res_value structures representing
  // the entry's parent (which is useful for reachability purposes).
  void collect_resource_values(uint32_t id,
                               std::vector<android::Res_value>* out);
  // Same as above, but if given list of "include_configs" is non-empty, results
  // written to out will be restricted to only these configs.
  void collect_resource_values(
      uint32_t id,
      std::vector<android::ResTable_config> include_configs,
      std::vector<android::Res_value>* out);
  bool is_valid_global_string_idx(size_t idx) const;
  // Convenience method to Read a string from the global string pool as standard
  // UTF-8.
  // ONLY USE FOR HUMAN READABLE PRINTING OR KNOWN SIMPLE STRINGS LIKE FILE
  // PATHS OR CLASS NAMES.
  std::string get_global_string_utf8s(size_t idx) const;
  // Get a representation of the underlying parsed file.
  TableEntryParser& get_parsed_table() { return m_table_parser; }
  android::ResStringPool& get_global_strings() { return m_global_strings; }

 private:
  TableEntryParser m_table_parser;
  android::ResStringPool m_global_strings;
  std::map<uint32_t, android::ResStringPool> m_key_strings;
  std::map<uint32_t, android::ResStringPool> m_type_strings;
};
} // namespace apk

class ResourcesArscFile : public ResourceTableFile {
 public:
  ResourcesArscFile(const ResourcesArscFile&) = delete;
  ResourcesArscFile& operator=(const ResourcesArscFile&) = delete;

  explicit ResourcesArscFile(const std::string& path);
  // Returns the string values of the resource(s) with the given name.
  // NOTE: For human readability, the returned strings in the vector will be
  // standard UTF-8 encoding!
  std::vector<std::string> get_resource_strings_by_name(
      const std::string& res_name);
  void remap_ids(const std::map<uint32_t, uint32_t>& old_to_remapped_ids);
  size_t obfuscate_resource_and_serialize(
      const std::vector<std::string>& resource_files,
      const std::map<std::string, std::string>& filepath_old_to_new,
      const UnorderedSet<uint32_t>& allowed_types,
      const UnorderedSet<std::string>& keep_resource_prefixes,
      const UnorderedSet<std::string>& keep_resource_specific) override;
  size_t serialize();

  size_t package_count() override;
  void collect_resid_values_and_hashes(
      const std::vector<uint32_t>& ids,
      std::map<size_t, std::vector<uint32_t>>* res_by_hash) override;
  bool resource_value_identical(uint32_t a_id, uint32_t b_id) override;
  void get_type_names(std::vector<std::string>* type_names) override;
  UnorderedSet<uint32_t> get_types_by_name(
      const UnorderedSet<std::string>& type_names) override;
  UnorderedSet<uint32_t> get_types_by_name_prefixes(
      const UnorderedSet<std::string>& type_name_prefixes) override;
  void delete_resource(uint32_t res_id) override;
  void remap_res_ids_and_serialize(
      const std::vector<std::string>& resource_files,
      const std::map<uint32_t, uint32_t>& old_to_new) override;
  void nullify_res_ids_and_serialize(
      const std::vector<std::string>& resource_files) override;
  void remap_reorder_and_serialize(
      const std::vector<std::string>& resource_files,
      const std::map<uint32_t, uint32_t>& old_to_new) override;
  void remap_file_paths_and_serialize(
      const std::vector<std::string>& resource_files,
      const UnorderedMap<std::string, std::string>& old_to_new) override;
  void finalize_resource_table(const ResourceConfig& config) override;
  std::vector<std::string> get_files_by_rid(
      uint32_t res_id,
      ResourcePathType path_type = ResourcePathType::DevicePath) override;
  void walk_references_for_resource(
      uint32_t resID,
      ResourcePathType path_type,
      UnorderedSet<uint32_t>* nodes_visited,
      UnorderedSet<std::string>* potential_file_paths) override;
  uint64_t resource_value_count(uint32_t res_id) override;
  void get_configurations(
      uint32_t package_id,
      const std::string& name,
      std::vector<android::ResTable_config>* configs) override;
  std::set<android::ResTable_config> get_configs_with_values(
      uint32_t id) override;
  // NOTE: this method will return values in standard UTF-8 to give a consistent
  // API/return values with BundleResources.
  void resolve_string_values_for_resource_reference(
      uint32_t ref, std::vector<std::string>* values) override;
  UnorderedMap<uint32_t, resources::InlinableValue>
  get_inlinable_resource_values() override;
  UnorderedSet<uint32_t> get_overlayable_id_roots() override;
  ~ResourcesArscFile() override;

  size_t get_length() const;

  apk::TableSnapshot& get_table_snapshot();

 private:
  void mark_file_closed();

  std::string m_path;
  RedexMappedFile m_f;
  size_t m_arsc_len;
  bool m_file_closed = false;
  std::unique_ptr<apk::TableSnapshot> m_table_snapshot;
};

class ApkResources : public AndroidResources {
 public:
  explicit ApkResources(const std::string& directory)
      : AndroidResources(directory),
        m_manifest(directory + "/AndroidManifest.xml") {}
  ~ApkResources() override;

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
  UnorderedSet<std::string> find_all_xml_files() override;
  std::vector<std::string> find_resources_files() override;
  std::string get_base_assets_dir() override;
  void obfuscate_xml_files(
      const UnorderedSet<std::string>& allowed_types,
      const UnorderedSet<std::string>& do_not_obfuscate_elements) override;

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
