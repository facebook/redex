/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

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

class AndroidResources {
 public:
  virtual boost::optional<int32_t> get_min_sdk() = 0;
  virtual ManifestClassInfo get_manifest_class_info() = 0;
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

  // Classnames present in native libraries (lib/*/*.so)
  std::unordered_set<std::string> get_native_classes();

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
