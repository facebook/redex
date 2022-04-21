/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/noncopyable.hpp>
#include <boost/optional.hpp>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "androidfw/ResourceTypes.h"

#include "RedexMappedFile.h"

const char* const ONCLICK_ATTRIBUTE = "android:onClick";
const char* const RES_DIRECTORY = "res";
const char* const OBFUSCATED_RES_DIRECTORY = "r";

const uint32_t PACKAGE_RESID_START = 0x7f000000;

/*
 * These are all the components which may contain references to Java classes in
 * their attributes.
 */
enum class ComponentTag {
  Activity,
  ActivityAlias,
  Provider,
  Receiver,
  Service,
};

/**
 * Indicate the value of the "exported" attribute of a component.
 */
enum class BooleanXMLAttribute {
  True,
  False,
  Undefined,
};

// Populate the ComponentTagInfo list of authority class names
void parse_authorities(const std::string& text,
                       std::unordered_set<std::string>* authority_classes);

struct ComponentTagInfo {
  ComponentTag tag;
  std::string classname;
  BooleanXMLAttribute is_exported;
  std::string permission;
  std::string protection_level;
  // Not defined on <provider>
  bool has_intent_filters{false};
  // Only defined on <provider>
  std::unordered_set<std::string> authority_classes;

  ComponentTagInfo(ComponentTag tag,
                   const std::string& classname,
                   BooleanXMLAttribute is_exported,
                   std::string permission,
                   std::string protection_level)
      : tag(tag),
        classname(classname),
        is_exported(is_exported),
        permission(std::move(permission)),
        protection_level(std::move(protection_level)) {}
};

struct ManifestClassInfo {
  std::unordered_set<std::string> application_classes;
  std::unordered_set<std::string> instrumentation_classes;
  std::vector<ComponentTagInfo> component_tags;
};

/**
 * Indicates whether or not a file path is from the perspective of the zip file
 * input to Redex, or the file path as meant to be read on device.
 */
enum class ResourcePathType {
  ZipPath,
  DevicePath,
};

class ResourceTableFile {
 public:
  virtual ~ResourceTableFile() {}

  virtual void collect_resid_values_and_hashes(
      const std::vector<uint32_t>& ids,
      std::map<size_t, std::vector<uint32_t>>* res_by_hash) = 0;
  virtual bool resource_value_identical(uint32_t a_id, uint32_t b_id) = 0;
  virtual std::unordered_set<uint32_t> get_types_by_name(
      const std::unordered_set<std::string>& type_names) = 0;
  virtual void delete_resource(uint32_t red_id) = 0;

  virtual void remap_res_ids_and_serialize(
      const std::vector<std::string>& resource_files,
      const std::map<uint32_t, uint32_t>& old_to_new) = 0;

  // Similar to above function, but reorder flags/entry/value data according to
  // old_to_new, as well as remapping references.
  virtual void remap_reorder_and_serialize(
      const std::vector<std::string>& resource_files,
      const std::map<uint32_t, uint32_t>& old_to_new) = 0;

  virtual void remap_file_paths_and_serialize(
      const std::vector<std::string>& resource_files,
      const std::unordered_map<std::string, std::string>& old_to_new) = 0;

  // Removes entries from string pool structures that are not referenced by
  // entries/values in the resource table
  virtual void remove_unreferenced_strings();

  // Returns any file paths from entries in the given ID. A non-existent ID or
  // an for which all values are not files will return an empty vector.
  // NOTE: callers should be resilient against duplicate file paths being
  // returned, which could concievably exist.
  virtual std::vector<std::string> get_files_by_rid(
      uint32_t res_id,
      ResourcePathType path_type = ResourcePathType::DevicePath) = 0;

  // Follows the reference links for a resource for all configurations. Outputs
  // all the nodes visited, as well as strings that may be additional resource
  // file paths.
  virtual void walk_references_for_resource(
      uint32_t resID,
      ResourcePathType path_type,
      std::unordered_set<uint32_t>* nodes_visited,
      std::unordered_set<std::string>* potential_file_paths) = 0;

  // Return the resource ids based on the given resource name.
  std::vector<uint32_t> get_res_ids_by_name(const std::string& name) const {
    if (name_to_ids.count(name)) {
      return name_to_ids.at(name);
    }
    return std::vector<uint32_t>{};
  }

  android::SortedVector<uint32_t> sorted_res_ids;
  std::map<uint32_t, std::string> id_to_name;
  std::map<std::string, std::vector<uint32_t>> name_to_ids;

 protected:
  ResourceTableFile() {}
};

class AndroidResources {
 public:
  virtual boost::optional<int32_t> get_min_sdk() = 0;

  virtual ManifestClassInfo get_manifest_class_info() = 0;

  // Given the xml file name, return the list of resource ids referred in xml
  // attributes.
  virtual std::unordered_set<uint32_t> get_xml_reference_attributes(
      const std::string& filename) = 0;

  // Rewrites all tag names/attribute values that are in the given map, for
  // every non-raw XML file in the directory.
  void rename_classes_in_layouts(
      const std::map<std::string, std::string>& rename_map);
  // Iterates through all layouts in the given directory. Adds all class names
  // to the output set, and allows for any specified attribute values to be
  // returned as well. Attribute names should specify their namespace, if any
  // (so android:onClick instead of just onClick)
  void collect_layout_classes_and_attributes(
      const std::unordered_set<std::string>& attributes_to_read,
      std::unordered_set<std::string>* out_classes,
      std::unordered_multimap<std::string, std::string>* out_attributes);

  // Same as above, for single file.
  virtual void collect_layout_classes_and_attributes_for_file(
      const std::string& file_path,
      const std::unordered_set<std::string>& attributes_to_read,
      std::unordered_set<std::string>* out_classes,
      std::unordered_multimap<std::string, std::string>* out_attributes) = 0;
  virtual std::unique_ptr<ResourceTableFile> load_res_table() = 0;
  virtual size_t remap_xml_reference_attributes(
      const std::string& filename,
      const std::map<uint32_t, uint32_t>& kept_to_remapped_ids) = 0;
  virtual std::unordered_set<std::string> find_all_xml_files() = 0;
  virtual std::vector<std::string> find_resources_files() = 0;
  virtual std::string get_base_assets_dir() = 0;
  // Classnames present in native libraries (lib/*/*.so)
  std::unordered_set<std::string> get_native_classes();

  const std::string& get_directory() { return m_directory; }

  virtual ~AndroidResources() {}

 protected:
  explicit AndroidResources(const std::string& directory)
      : m_directory(directory) {}

  virtual std::vector<std::string> find_res_directories() = 0;
  virtual std::vector<std::string> find_lib_directories() = 0;

  // Mutate the given file based on the rename map, returning whether or not it
  // worked with some potentially meaningless out params for size metrics.
  virtual bool rename_classes_in_layout(
      const std::string& file_path,
      const std::map<std::string, std::string>& rename_map,
      size_t* out_num_renamed) = 0;

  const std::string& m_directory;
};

std::unique_ptr<AndroidResources> create_resource_reader(
    const std::string& directory);

// For testing only!
std::unordered_set<std::string> extract_classes_from_native_lib(
    const std::string& lib_contents);

std::unordered_set<std::string> get_files_by_suffix(
    const std::string& directory, const std::string& suffix);
std::unordered_set<std::string> get_xml_files(const std::string& directory);
// Checks if the file is in a res/raw folder. Such a file won't be considered
// for resource remapping, class name extraction, etc. These files don't follow
// binary XML format, and thus are out of scope for many optimizations.
bool is_raw_resource(const std::string& filename);

// Convenience method for copying values in a multimap to a set, for a
// particular key.
std::set<std::string> multimap_values_to_set(
    const std::unordered_multimap<std::string, std::string>& map,
    const std::string& key);

const int TYPE_INDEX_BIT_SHIFT = 16;
const int PACKAGE_INDEX_BIT_SHIFT = 24;
const uint32_t PACKAGE_MASK_BIT = 0xFF000000;
const uint32_t TYPE_MASK_BIT = 0x00FF0000;
const uint32_t ENTRY_MASK_BIT = 0x0000FFFF;
