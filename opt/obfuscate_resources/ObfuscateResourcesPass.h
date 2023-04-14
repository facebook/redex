/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <set>
#include <string>

#include "Pass.h"

/**
 * This Pass will generate a resource id to resource name file at beginning.
 *
 * Then, depending on pass settings it can:
 * - Anonymize resource names by setting resource names to "(name removed)".
 * - Shorten file paths in the zip and resource table for certain file types.
 * - Manipulate binary xml files to remove string pool data unlikely to be used.

 * NOTE: This pass may increase the size of the resource table, under the
 * assumption that a full cleanup is later done via the
 * "finalize_resource_table" global option. This later finalize step will
 * actually remove strings that become unused as a result of this pass (and
 * other passes).
 */
class ObfuscateResourcesPass : public Pass {
 public:
  ObfuscateResourcesPass() : Pass("ObfuscateResourcesPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::names;
    return {
        {NoInitClassInstructions, {.preserves = true}},
        {HasSourceBlocks, {.preserves = true}},
    };
  }

  void bind_config() override {
    // Resource type names (i.e. color, dimen, etc) that may have the names of
    // entries in that type removed (replaced with a dummy value).
    bind("allow_types_prefixes", {}, m_name_obfuscation_allowed_types);
    // Exceptions to the above config, any resource name in an allowed type
    // starting with a prefix will be kept.
    bind("keep_resource_prefixes", {}, m_keep_resource_name_prefixes);
    // We might want to avoid changing certain file's path
    bind("do_not_obfuscate_file", {}, m_keep_resource_file_names);
    // Resource type names (i.e. drawable, layout) for which obfuscation of xml
    // attributes should take place.
    bind("xml_obfuscation_allowed_types", {}, m_xml_obfuscation_allowed_types);
    // Exceptions to the above config, any xml element with a name in this set
    // will not have its attributes obfuscated.
    bind("do_not_obfuscate_elements", {}, m_do_not_obfuscate_elements);

    // Whether or not to remove resource identifier names. If true, the string
    // index for "foo" (from R.layout.foo) will be rewritten.
    bind("obfuscate_resource_name", false, m_obfuscate_resource_name);
    // Same as above, but special flag for id type. Certain instrumentation test
    // frameworks may, for correct operation, require id names. Flip this off if
    // needed.
    bind("obfuscate_id_name", false, m_obfuscate_id_name);
    // If true, resource file names, like "res/layout/activity_main.xml" will be
    // shortened in the zip and resource table like "r/aa.xml" to save some
    // bytes.
    bind("obfuscate_resource_file", true, m_obfuscate_resource_file);
    // If true, xml files of certain types (see xml_obfuscation_allowed_types
    // above) will have their string pools manipulated to remove likely unused
    // data.
    bind("obfuscate_xml_attributes", false, m_obfuscate_xml_attributes);
    // If true, resource ids that are found in const-string literals, or
    // attribute values in .xml files will be kept. This is intentionally very
    // conservative.
    bind("keep_resource_names_from_string_literals", false,
         m_keep_resource_names_from_string_literals);
    // A set of class name prefixes / method name prefixes, for which any string
    // constant found will be allowed for resource name obfuscation, even if it
    // happens to be a valid resource name (makes for easier test cases, tuning,
    // etc).
    bind("code_references_okay_to_obfuscate", {},
         m_code_references_okay_to_obfuscate);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  std::unordered_set<std::string> m_name_obfuscation_allowed_types;
  std::unordered_set<std::string> m_keep_resource_name_prefixes;
  std::unordered_set<std::string> m_xml_obfuscation_allowed_types;
  std::unordered_set<std::string> m_do_not_obfuscate_elements;
  std::unordered_set<std::string> m_keep_resource_file_names;
  std::unordered_set<std::string> m_code_references_okay_to_obfuscate;
  bool m_obfuscate_resource_name;
  bool m_obfuscate_resource_file;
  bool m_obfuscate_id_name;
  bool m_obfuscate_xml_attributes;
  bool m_keep_resource_names_from_string_literals;
};
