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
