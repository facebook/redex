/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "androidfw/ResourceTypes.h"

#include "RedexMappedFile.h"
#include "RedexResources.h"

class ApkResources : public AndroidResources {
 public:
  explicit ApkResources(const std::string& directory)
      : AndroidResources(directory),
        m_manifest(directory + "/AndroidManifest.xml") {}
  boost::optional<int32_t> get_min_sdk() override;
  ManifestClassInfo get_manifest_class_info() override;
  // Replaces all strings in the ResStringPool for the given file with their
  // replacements. Writes all changes to disk, clobbering the given file.
  // Same return codes as replace_in_xml_string_pool.
  int rename_classes_in_layout(
      const std::string& file_path,
      const std::map<std::string, std::string>& rename_map,
      size_t* out_num_renamed,
      ssize_t* out_size_delta);
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
  void rename_classes_in_layouts(
      const std::map<std::string, std::string>& rename_map) override;

  int inline_xml_reference_attributes(
      const std::string& filename,
      const std::map<uint32_t, android::Res_value>& id_to_inline_value);
  void remap_xml_reference_attributes(
      const std::string& filename,
      const std::map<uint32_t, uint32_t>& kept_to_remapped_ids);

 private:
  const std::string m_manifest;
};

class ResourcesArscFile {
 public:
  ResourcesArscFile(const ResourcesArscFile&) = delete;
  ResourcesArscFile& operator=(const ResourcesArscFile&) = delete;

  android::ResTable res_table;
  android::SortedVector<uint32_t> sorted_res_ids;
  std::map<uint32_t, std::string> id_to_name;
  std::map<std::string, std::vector<uint32_t>> name_to_ids;

  explicit ResourcesArscFile(const std::string& path);
  std::vector<std::string> get_resource_strings_by_name(
      const std::string& res_name);
  void remap_ids(const std::map<uint32_t, uint32_t>& old_to_remapped_ids);
  std::unordered_set<uint32_t> get_types_by_name(
      const std::unordered_set<std::string>& type_names);
  size_t serialize();
  ~ResourcesArscFile();

  size_t get_length() const;

 private:
  RedexMappedFile m_f;
  size_t m_arsc_len;
  bool m_file_closed = false;
};
