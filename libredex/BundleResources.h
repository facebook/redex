/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

#include "protores/Resources.pb.h"
#include <google/protobuf/repeated_field.h>

using ConfigValues = google::protobuf::RepeatedPtrField<aapt::pb::ConfigValue>;

class BundleResources : public AndroidResources {
 public:
  explicit BundleResources(const std::string& directory)
      : AndroidResources(directory) {}
  boost::optional<int32_t> get_min_sdk() override;
  ManifestClassInfo get_manifest_class_info() override;
  void collect_layout_classes_and_attributes_for_file(
      const std::string& file_path,
      const std::unordered_set<std::string>& attributes_to_read,
      std::unordered_set<std::string>* out_classes,
      std::unordered_multimap<std::string, std::string>* out_attributes)
      override;
  void collect_resource_data_for_file();
  std::unordered_set<uint32_t> get_types_by_name(
      const std::unordered_set<std::string>& type_names);
  void alter_resource_data_for_file(
      const std::set<uint32_t>& ids_to_remove,
      const std::map<uint32_t, uint32_t>& old_to_new);
  size_t remap_xml_reference_attributes(
      const std::string& filename,
      const std::map<uint32_t, uint32_t>& kept_to_remapped_ids);
  bool resource_value_identical(uint32_t a_id, uint32_t b_id);
  size_t get_hash_from_values(const ConfigValues& config_values);
  std::vector<std::string> find_resources_pb_files();
  std::vector<uint32_t> get_res_ids_by_name(const std::string& name);
  void clear_restable();
  const std::map<uint32_t, std::string>& get_res_id_to_name() const {
    return m_res_id_to_name;
  }
  const std::map<uint32_t, const ConfigValues>& get_res_id_to_configvalue()
      const {
    return m_res_id_to_configvalue;
  }

 protected:
  std::vector<std::string> find_res_directories() override;
  std::vector<std::string> find_lib_directories() override;

  bool rename_classes_in_layout(
      const std::string& file_path,
      const std::map<std::string, std::string>& rename_map,
      size_t* out_num_renamed) override;

 private:
  uint32_t m_package_id = 0xFFFFFFFF;
  std::map<uint32_t, std::string> m_type_id_to_names;
  std::map<uint32_t, std::string> m_res_id_to_name;
  android::SortedVector<uint32_t> m_sorted_res_ids;
  std::unordered_set<uint32_t> m_existed_res_ids;
  std::map<std::string, std::vector<uint32_t>> m_name_to_res_ids;
  std::map<uint32_t, const ConfigValues> m_res_id_to_configvalue;
};

#endif // HAS_PROTOBUF
