/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

/*
 * This pass identifies fields that are never read from and deletes all writes
 * to them. Similarly, all fields that are never written to and do not have
 * a non-zero static value get all of their read instructions replaced by
 * const 0 instructions.
 *
 * This pass relies on RemoveUnreachablePass running afterward to remove the
 * definitions of those fields entirely.
 *
 * Possible future work: This could be extended to eliminate fields that are
 * only used in non-escaping contexts.
 *
 * NOTE: Removing writes to fields may affect the life-time of an object, if all
 * other references to it are weak. Thus, this is a somewhat unsafe, or at least
 * potentially behavior altering optimization.
 */
namespace remove_unused_fields {

constexpr const char* REMOVED_FIELDS_FILENAME = "redex-removed-fields.txt";

struct Config {
  bool remove_unread_fields;
  bool remove_unwritten_fields;
  bool remove_zero_written_fields;
  bool remove_vestigial_objects_written_fields;
  std::unordered_set<const DexType*> blocklist_types;
  std::unordered_set<const DexType*> blocklist_classes;
  boost::optional<std::unordered_set<DexField*>> allowlist;
};

class PassImpl : public Pass {
 public:
  PassImpl() : Pass("RemoveUnusedFieldsPass") {}

  void bind_config() override {
    bind("remove_unread_fields", true, m_config.remove_unread_fields);
    bind("remove_unwritten_fields", true, m_config.remove_unwritten_fields);
    bind("remove_zero_written_fields",
         true,
         m_config.remove_zero_written_fields);
    bind("remove_vestigial_objects_written_fields", true,
         m_config.remove_vestigial_objects_written_fields);
    bind("blocklist_types",
         {},
         m_config.blocklist_types,
         "Fields with these types will never be removed.",
         Configurable::bindflags::types::warn_if_unresolvable);
    bind("blocklist_classes",
         {},
         m_config.blocklist_classes,
         "Fields in these classes will never be removed.");

    // These options make it a bit more convenient to bisect the list of removed
    // fields to isolate one that's causing issues.
    bind("export_removed",
         false,
         m_export_removed,
         "Write all removed fields to " + std::string(REMOVED_FIELDS_FILENAME));

    bind("allowlist_file",
         {boost::none},
         m_allowlist_file,
         "If specified, RMUF will only remove fields listed in this file. You "
         "can use the file created by the `export_removed` option as input "
         "here.");
    after_configuration([&]() {
      if (!m_allowlist_file) {
        return;
      }
      std::ifstream ifs(*m_allowlist_file);
      if (!ifs) {
        std::cerr << "RMUF: failed to open allowlist file at "
                  << *m_allowlist_file << std::endl;
        return;
      }
      m_config.allowlist = std::unordered_set<DexField*>();
      std::string field_str;
      while (std::getline(ifs, field_str)) {
        auto* field = DexField::get_field(field_str);
        if (field == nullptr || !field->is_def()) {
          std::cerr << "RMUF: failed to resolve " << field_str << std::endl;
        }
        m_config.allowlist->emplace(static_cast<DexField*>(field));
      }
      std::cerr << "RMUF: Found " << m_config.allowlist->size()
                << " allowed fields" << std::endl;
    });
  }

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;

 private:
  Config m_config;
  boost::optional<std::string> m_allowlist_file;
  bool m_export_removed;
};

} // namespace remove_unused_fields
