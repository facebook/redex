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
#include <boost/graph/adjacency_list.hpp>
#include <boost/noncopyable.hpp>
#include <boost/optional.hpp>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "androidfw/ResourceTypes.h"

#include "Debug.h"
#include "DeterministicContainers.h"
#include "DexUtil.h"
#include "GlobalConfig.h"
#include "RedexMappedFile.h"

const char* const ONCLICK_ATTRIBUTE = "android:onClick";
const char* const RES_DIRECTORY = "res";
const char* const OBFUSCATED_RES_DIRECTORY = "r";
const char* const RESOURCE_NAME_REMOVED = "(name removed)";

const uint32_t PACKAGE_RESID_START = 0x7f000000;
const uint32_t APPLICATION_PACKAGE = 0x7f;

namespace resources {
// Use case specific options for traversing and establishing reachable roots.
struct ReachabilityOptions {
  bool assume_id_inlined{false};
  bool check_string_for_name{false};
  bool granular_style_reachability{false};
  std::vector<std::string> assume_reachable_prefixes;
  UnorderedSet<std::string> assume_reachable_names;
  UnorderedSet<std::string> disallowed_types;
};

// Holder object for details about a type that is pending creation.
struct TypeDefinition {
  uint32_t package_id;
  uint8_t type_id;
  std::string name;
  std::vector<android::ResTable_config*> configs;
  std::vector<uint32_t> source_res_ids;
};

// List of attribute names (without namespace) in xml documents for which we
// should hunt for class names. This is intentionally broad, as narrowly
// targeting specific element names would require analyzing parent element names
// (example: children of
// https://developer.android.com/reference/androidx/coordinatorlayout/widget/CoordinatorLayout).
const inline std::set<std::string> POSSIBLE_CLASS_ATTRIBUTES = {
    "actionViewClass", "argType",       "class", "controller",
    "layout_behavior", "layoutManager", "name",  "targetClass",
};
// Returns false if there is no dot or it's not a Java identifier.
bool valid_xml_element(const std::string& ident);

inline std::string fully_qualified_external_name(
    const std::string& package_name, const std::string& class_name) {
  if (class_name.empty()) {
    return class_name;
  }
  if (class_name.at(0) == '.') {
    return java_names::external_to_internal(package_name + class_name);
  }
  return java_names::external_to_internal(class_name);
}

// If the given type name is a custom type, i.e. "dimen.2" return the actual
// type it represents, in this example "dimen". Otherwise string is returned as
// is.
inline std::string type_name_from_possibly_custom_type_name(
    const std::string& type_name) {
  auto end_pos = type_name.rfind('.');
  if (end_pos == std::string::npos) {
    return type_name;
  } else {
    return type_name.substr(0, end_pos);
  }
}

struct StringOrReference {
  const std::string str;
  const uint32_t ref;

  explicit StringOrReference(const std::string& value) : str(value), ref(0) {}
  explicit StringOrReference(const uint32_t& value) : ref(value) {}

  bool is_reference() const { return ref != 0; }

  bool possible_java_identifier() const {
    if (ref != 0) {
      return true;
    }
    return java_names::is_identifier(str);
  }

  bool operator==(const StringOrReference& that) const {
    return str == that.str && ref == that.ref;
  }
};

struct InlinableValue {
  uint8_t type;
  uint32_t uint_value;
  bool bool_value;
  std::string string_value;
};

struct StringOrReferenceHasher {
  size_t operator()(const StringOrReference& val) const {
    size_t seed = 0;
    boost::hash_combine(seed, val.str);
    boost::hash_combine(seed, val.ref);
    return seed;
  }
};

using StringOrReferenceSet =
    UnorderedSet<StringOrReference, StringOrReferenceHasher>;

// Helper for parsing resources in "tools:keep" part of xml file
UnorderedSet<std::string> parse_keep_xml_file(const std::string& xml_file_path);

// Basic scaffolding to represent styles and their hierarchy in the application.
// This representation is meant to be common between .apk and .aab inputs, which
// is why android::ResTable_config is emitted as a copy here (since .pb
// representation of config can be easily converted to the .arsc form, for a
// common interface between the two).
struct StyleResource {
  struct Value {
    struct Span {
      Span(std::string tag, uint32_t first_char, uint32_t last_char)
          : tag(std::move(tag)), first_char(first_char), last_char(last_char) {}
      std::string tag;
      uint32_t first_char;
      uint32_t last_char;

      bool operator==(const Span& other) const {
        if (tag != other.tag) {
          return false;
        }
        if (first_char != other.first_char) {
          return false;
        }
        return last_char == other.last_char;
      }
    };

    Value(uint8_t dt, uint32_t bytes) : data_type(dt), value_bytes(bytes) {}
    Value(uint8_t dt, const std::string& str)
        : data_type(dt), value_string(str) {}
    Value(uint8_t dt, const std::string& str, std::vector<Span> styled)
        : data_type(dt), value_string(str), styled_string(std::move(styled)) {}

    bool operator==(const Value& other) const {
      if (data_type != other.data_type) {
        return false;
      }
      if (value_bytes != other.value_bytes) {
        return false;
      }
      if (value_string != other.value_string) {
        return false;
      }
      return styled_string == other.styled_string;
    }

    uint8_t get_data_type() const { return data_type; }
    uint32_t get_value_bytes() const { return value_bytes; }
    const boost::optional<std::string>& get_value_string() const {
      return value_string;
    }
    const std::vector<Span>& get_styled_string() const { return styled_string; }

   private:
    uint8_t data_type{0};
    uint32_t value_bytes{0};
    boost::optional<std::string> value_string{boost::none};
    std::vector<Span> styled_string;
  };

  using AttrMap = std::map<uint32_t, Value>;

  uint32_t id{0};
  android::ResTable_config config{};
  uint32_t parent{0};
  AttrMap attributes;

  bool operator==(const StyleResource& other) const {
    if (parent != other.parent) {
      return false;
    }
    return attributes == other.attributes;
  }
};

// Map of ID to parsed style information (one ID can map to many due to
// different configs, i.e. default / night mode / land, etc).
using StyleMap = std::unordered_map<uint32_t, std::vector<StyleResource>>;

struct StyleInfo {
  // Graph of style resource IDs as vertex, edge to denote a style's parent (if
  // that parent is also defined in the application). Note that styles which
  // inherit from framework styles will lack an outbound edge.
  struct Node {
    uint32_t id{0};
  };
  using Graph =
      boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS, Node>;
  using vertex_t = boost::graph_traits<Graph>::vertex_descriptor;
  Graph graph;
  // Actual representation of the parsed style information from the application.
  StyleMap styles;
  // Maps resource ID to the vertex in the graph.
  std::unordered_map<uint32_t, resources::StyleInfo::vertex_t> id_to_vertex;
  // Returns information from the graph as a .dot format, for visualization.
  // Optionally this can exclude nodes that have no outgoing/inbound edges which
  // might not be interesting to look at.
  std::string print_as_dot(bool exclude_nodes_with_no_edges = false);
  // As above, "stringify" function is to convert the ID to a readable name. By
  // default an implementation that prints the ID as hex will be used.
  std::string print_as_dot(
      const std::function<std::string(uint32_t)>& stringify,
      const UnorderedMap<uint32_t, UnorderedMap<std::string, std::string>>&
          node_options,
      bool exclude_nodes_with_no_edges = false);

  // Returns the set of root vertices in the graph. These are typically the
  // top-level styles in the style hierarchy.
  UnorderedSet<vertex_t> get_roots() const;

  // Gets all children of the given resource ID.
  std::vector<uint32_t> get_children(uint32_t resource_id) const;

  // Gets parent of the given resource ID.
  std::optional<uint32_t> get_unambiguous_parent(uint32_t resource_id) const;
};

// Modification specification for styles in APK and App Bundle containers.
// This structure defines operations that can be performed on styles during
// serialization.
struct StyleModificationSpec {
  enum class ModificationType {
    ADD_ATTRIBUTE,
    REMOVE_ATTRIBUTE,
    DELETE_STYLE,
    UPDATE_PARENT_ADD_ATTRIBUTES
  };

  struct Modification {
    ModificationType type;

    uint32_t resource_id{0};
    std::optional<uint32_t> attribute_id{std::nullopt};
    std::optional<StyleResource::Value> value{std::nullopt};
    std::optional<uint32_t> parent_id{std::nullopt};
    UnorderedMap<uint32_t, StyleResource::Value> values;

    explicit Modification(uint32_t resource_id)
        : type(ModificationType::DELETE_STYLE), resource_id{resource_id} {}
    Modification(uint32_t resource_id, uint32_t attr_id)
        : type(ModificationType::REMOVE_ATTRIBUTE),
          resource_id{resource_id},
          attribute_id(attr_id) {}
    Modification(uint32_t resource_id,
                 uint32_t attr_id,
                 const StyleResource::Value& val)
        : type(ModificationType::ADD_ATTRIBUTE),
          resource_id{resource_id},
          attribute_id(attr_id),
          value(val) {}
    Modification(uint32_t resource_id,
                 uint32_t parent_id,
                 UnorderedMap<uint32_t, StyleResource::Value> values)
        : type(ModificationType::UPDATE_PARENT_ADD_ATTRIBUTES),
          resource_id(resource_id),
          parent_id(parent_id),
          values(std::move(values)) {}
  };

  std::vector<Modification> modifications;
};

using ResourceAttributeMap =
    UnorderedMap<uint32_t,
                 UnorderedMap<uint32_t, StyleModificationSpec::Modification>>;

// Helper for dealing with differences in character encoding between .arsc and
// .pb files.
std::string convert_utf8_to_mutf8(const std::string& input);
// Given a map of a id which holds a reference value, and the id that the
// reference points to, along with all the past found inlinable values, for each
// id in past_refs, if it is inlinable, adds it to inlinable_resources with the
// value that it's reference holds
void resources_inlining_find_refs(
    const UnorderedMap<uint32_t, uint32_t>& past_refs,
    UnorderedMap<uint32_t, resources::InlinableValue>* inlinable_resources);
} // namespace resources

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
                       UnorderedSet<std::string>* authority_classes);

struct ComponentTagInfo {
  ComponentTag tag;
  std::string classname;
  BooleanXMLAttribute is_exported;
  std::string permission;
  std::string protection_level;
  // Not defined on <provider>
  bool has_intent_filters{false};
  // Only defined on <provider>
  UnorderedSet<std::string> authority_classes;

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
  UnorderedSet<std::string> application_classes;
  UnorderedSet<std::string> instrumentation_classes;
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

  virtual size_t package_count() = 0;
  virtual void collect_resid_values_and_hashes(
      const std::vector<uint32_t>& ids,
      std::map<size_t, std::vector<uint32_t>>* res_by_hash) = 0;
  virtual bool resource_value_identical(uint32_t a_id, uint32_t b_id) = 0;
  // Fill the given vector with the names of types in the resource table, using
  // .apk conventions for numbering such that the zeroth element of the vector
  // is the name of type ID 0x1, 1st element of the vector is the name of type
  // ID 0x2, etc. To make this numbering scheme work, non-contiguous type IDs
  // will need to put placeholder/empty strings in the output vector.
  // This API is wonky, but meant to mimic iterating over the .arsc file type
  // string pool and how that would behave.
  virtual void get_type_names(std::vector<std::string>* type_names) = 0;
  // Return type ids for the given set of type names. Type ids will be shifted
  // to TT0000 range, so type 0x1 will be returned as 0x10000 (for ease of
  // comparison with resource IDs).
  virtual UnorderedSet<uint32_t> get_types_by_name(
      const UnorderedSet<std::string>& type_names) = 0;
  // Same as above, return values will be given in no particular order.
  UnorderedSet<uint32_t> get_types_by_name(
      const std::vector<std::string>& type_names) {
    UnorderedSet<std::string> set;
    set.insert(type_names.begin(), type_names.end());
    return get_types_by_name(set);
  }
  virtual UnorderedSet<uint32_t> get_types_by_name_prefixes(
      const UnorderedSet<std::string>& type_name_prefixes) = 0;
  virtual void delete_resource(uint32_t red_id) = 0;

  virtual void remap_res_ids_and_serialize(
      const std::vector<std::string>& resource_files,
      const std::map<uint32_t, uint32_t>& old_to_new) = 0;
  // Instead of remapping deleted resource ids, we nullify them.
  virtual void nullify_res_ids_and_serialize(
      const std::vector<std::string>& resource_files) = 0;

  // Similar to above function, but reorder flags/entry/value data according to
  // old_to_new, as well as remapping references.
  virtual void remap_reorder_and_serialize(
      const std::vector<std::string>& resource_files,
      const std::map<uint32_t, uint32_t>& old_to_new) = 0;

  virtual void remap_file_paths_and_serialize(
      const std::vector<std::string>& resource_files,
      const UnorderedMap<std::string, std::string>& old_to_new) = 0;
  // Rename qualified resource names that are in allowed type, and are not in
  // the specific list of resource names to keep and don't have a prefix in the
  // keep_resource_prefixes set. All such resource names will be rewritten to
  // "(name removed)". Also, rename filepaths according to filepath_old_to_new.
  virtual size_t obfuscate_resource_and_serialize(
      const std::vector<std::string>& resource_files,
      const std::map<std::string, std::string>& filepath_old_to_new,
      const UnorderedSet<uint32_t>& allowed_types,
      const UnorderedSet<std::string>& keep_resource_prefixes,
      const UnorderedSet<std::string>& keep_resource_specific) = 0;

  // Removes entries from string pool structures that are not referenced by
  // entries/values in the resource table and other structural changes that are
  // better left until all passes have run.
  virtual void finalize_resource_table(const ResourceConfig& config);

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
      const ResourcePathType& path_type,
      const resources::ReachabilityOptions& reachability_options,
      UnorderedSet<uint32_t>* nodes_visited,
      UnorderedSet<std::string>* potential_file_paths) = 0;

  // Mainly used by test to check if a resource has been nullified
  virtual uint64_t resource_value_count(uint32_t res_id) = 0;

  // For a given package and type name (i.e. "drawable", "layout", etc) return
  // the configurations of that type. Data that is outputted may require
  // conversion, which will happen internally, so do not use reference equality
  // on the result.
  virtual void get_configurations(
      uint32_t package_id,
      const std::string& name,
      std::vector<android::ResTable_config>* configs) = 0;

  // For a given resource ID, return the configs for which the value is nonempty
  virtual std::set<android::ResTable_config> get_configs_with_values(
      uint32_t id) = 0;

  // For a given resource ID, find all string values that the ID could represent
  // across all configurations (including chasing down references).
  // NOTE: in case of supplimental characters in string values, UTF-8 standard
  // encoding will be returned, so that the caller will have a consistent
  // behavior regardless of apk / aab container formats.
  virtual void resolve_string_values_for_resource_reference(
      uint32_t ref, std::vector<std::string>* values) = 0;

  virtual UnorderedMap<uint32_t, resources::InlinableValue>
  get_inlinable_resource_values() = 0;

  // Returns a set of IDs that are overlayable, to be used as reachability roots
  virtual UnorderedSet<uint32_t> get_overlayable_id_roots() = 0;

  // Builds a map of resource ID -> information about style resources in all
  // configurations.
  virtual resources::StyleMap get_style_map() = 0;

  // Deletes referenced attribute/value in android app
  virtual void apply_attribute_removals(
      const std::vector<resources::StyleModificationSpec::Modification>&
          modifications,
      const std::vector<std::string>& resources_pb_paths) = 0;

  // Adds referenced attribute/value in android app
  virtual void apply_attribute_additions(
      const std::vector<resources::StyleModificationSpec::Modification>&
          modifications,
      const std::vector<std::string>& resources_pb_paths) = 0;

  // Builds a graph of all styles in the application, with outgoing edges to
  // the parent of each style.
  resources::StyleInfo load_style_info();

  // Takes effect during serialization. Appends a new type with the given
  // details (id, name) to the package. It will contain types with the given
  // configs and use existing resource entry/value data of "source_res_ids" to
  // populate this new type. Actual type data in the resulting file will be
  // emitted in the order as the given configs.
  void define_type(uint32_t package_id,
                   uint8_t type_id,
                   const std::string& name,
                   const std::vector<android::ResTable_config*>& configs,
                   const std::vector<uint32_t>& source_res_ids) {
    always_assert_log((package_id & 0xFFFFFF00) == 0,
                      "package_id expected to have low byte set; got 0x%x",
                      package_id);
    resources::TypeDefinition def{package_id, type_id, name, configs,
                                  source_res_ids};
    m_added_types.emplace_back(std::move(def));
  }

  // Return the resource ids based on the given resource name.
  std::vector<uint32_t> get_res_ids_by_name(const std::string& name) const {
    if (name_to_ids.count(name)) {
      return name_to_ids.at(name);
    }
    return std::vector<uint32_t>{};
  }

  // Checks if the given type id is of the given name, coalescing custom type
  // names.
  bool is_type_named(uint8_t type_id, const std::string& type_name) const {
    auto search = m_application_type_ids_to_names.find(type_id);
    return search != m_application_type_ids_to_names.end() &&
           search->second == type_name;
  }

  std::vector<uint32_t> sorted_res_ids;
  std::map<uint32_t, std::string> id_to_name;
  std::map<std::string, std::vector<uint32_t>> name_to_ids;
  bool m_nullify_removed{false};
  // Pending changes to take effect during serialization
  UnorderedSet<uint32_t> m_ids_to_remove;
  std::vector<resources::TypeDefinition> m_added_types;
  // type ids to coalesced type names (i.e. type id for "style.2" will be mapped
  // to "style" here).
  UnorderedMap<uint8_t, std::string> m_application_type_ids_to_names;

 protected:
  ResourceTableFile() {}
};

class AndroidResources {
 public:
  virtual boost::optional<int32_t> get_min_sdk() = 0;

  virtual ManifestClassInfo get_manifest_class_info() = 0;
  virtual boost::optional<std::string> get_manifest_package_name() = 0;

  virtual UnorderedSet<std::string> get_service_loader_classes() = 0;

  // Given the xml file name, return the list of resource ids referred in xml
  // attributes.
  virtual UnorderedSet<uint32_t> get_xml_reference_attributes(
      const std::string& filename) = 0;

  // Rewrites all tag names/attribute values that are in the given map, for
  // every non-raw XML file in the directory.
  void rename_classes_in_layouts(
      const std::map<std::string, std::string>& rename_map);

  // Iterates through all layouts in the given directory. Adds possible class
  // name candidates to the out parameter, and allows for any specified
  // attribute values to be returned as well. Returned values may or may not
  // refer to real classes, and will be given in external name form (so
  // "com.facebook.Foo" not "Lcom/facebook/Foo;"). Attribute names that are to
  // be read should specify their namespace, if any (so android:onClick instead
  // of just onClick). Any references encountered in attribute values will be
  // resolved against the resource table, and all possible discovered values (in
  // all configs) will be included in the output.
  void collect_layout_classes_and_attributes(
      const UnorderedSet<std::string>& attributes_to_read,
      UnorderedSet<std::string>* out_classes,
      std::unordered_multimap<std::string, std::string>* out_attributes);

  // Same as above, for single file.
  virtual void collect_layout_classes_and_attributes_for_file(
      const std::string& file_path,
      const UnorderedSet<std::string>& attributes_to_read,
      resources::StringOrReferenceSet* out_classes,
      std::unordered_multimap<std::string, resources::StringOrReference>*
          out_attributes) = 0;
  // Similar to collect_layout_classes_and_attributes, but less focused to cover
  // custom View subclasses that might be doing interesting things with string
  // values
  void collect_xml_attribute_string_values(UnorderedSet<std::string>* out);
  // As above, for single file.
  virtual void collect_xml_attribute_string_values_for_file(
      const std::string& file_path, UnorderedSet<std::string>* out) = 0;
  // Transforms element names in the given map to be <view> elements with their
  // class name specified fully qualified. Out param indicates the number of
  // elements that were changed.
  virtual void fully_qualify_layout(
      const UnorderedMap<std::string, std::string>& element_to_class_name,
      const std::string& file_path,
      size_t* changes) = 0;

  virtual std::unique_ptr<ResourceTableFile> load_res_table() = 0;
  virtual size_t remap_xml_reference_attributes(
      const std::string& filename,
      const std::map<uint32_t, uint32_t>& kept_to_remapped_ids) = 0;
  virtual UnorderedSet<std::string> find_all_xml_files() = 0;
  virtual std::vector<std::string> find_resources_files() = 0;
  virtual std::string get_base_assets_dir() = 0;
  // For drawable/layout .xml files, remove/shorten attribute names where
  // possible. Any file with an element name in the given set will be kept
  // intact by convention (this method will be overly cautious when applying
  // keeps).
  virtual void obfuscate_xml_files(
      const UnorderedSet<std::string>& allowed_types,
      const UnorderedSet<std::string>& do_not_obfuscate_elements) = 0;
  bool can_obfuscate_xml_file(const UnorderedSet<std::string>& allowed_types,
                              const std::string& dirname);
  // Classnames present in native libraries (lib/*/*.so)
  UnorderedSet<std::string> get_native_classes();
  // Sets up BundleConfig.pb file with relevant options for resource
  // optimizations that need to executed by bundletool/aapt2.
  virtual void finalize_bundle_config(const ResourceConfig& config);

  const std::string& get_directory() { return m_directory; }

  UnorderedSet<std::string> get_all_keep_resources();

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

UnorderedSet<std::string> get_service_loader_classes_helper(
    const std::string& path_dir);

// For testing only!
UnorderedSet<std::string> extract_classes_from_native_lib(
    const std::string& lib_contents);

UnorderedSet<std::string> get_files_by_suffix(const std::string& directory,
                                              const std::string& suffix);
UnorderedSet<std::string> get_xml_files(const std::string& directory);
// Checks if the file is in a res/raw folder. Such a file won't be considered
// for resource remapping, class name extraction, etc. These files don't follow
// binary XML format, and thus are out of scope for many optimizations.
bool is_raw_resource(const std::string& filename);

std::string configs_to_string(
    const std::set<android::ResTable_config>& configs);

const int TYPE_INDEX_BIT_SHIFT = 16;
const int PACKAGE_INDEX_BIT_SHIFT = 24;
const uint32_t PACKAGE_MASK_BIT = 0xFF000000;
const uint32_t TYPE_MASK_BIT = 0x00FF0000;
const uint32_t ENTRY_MASK_BIT = 0x0000FFFF;
