/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef _FB_ANDROID_VISITOR_H
#define _FB_ANDROID_VISITOR_H

#include "androidfw/ResourceTypes.h"

namespace arsc {
// Class to traverse various structures in resources.arsc file. Callers should
// override various methods that are relevant for their use case. Mutating data
// is allowed, though traversal logic will not expect anything data to change
// its size (so only use this to make simple edits like changing IDs, changing
// string pool references, etc).
class ResourceTableVisitor {
 public:
  virtual bool visit(void* data, size_t len);
  virtual bool visit_table(android::ResTable_header* table);
  virtual bool visit_global_strings(android::ResStringPool_header* pool);
  // There can be many packages
  virtual bool visit_package(android::ResTable_package* package);
  virtual bool visit_type_strings(android::ResTable_package* package,
                                  android::ResStringPool_header* pool);
  virtual bool visit_key_strings(android::ResTable_package* package,
                                 android::ResStringPool_header* pool);
  // One type spec will exist per type in the package.
  virtual bool visit_type_spec(android::ResTable_package* package,
                               android::ResTable_typeSpec* type_spec);
  // Per type spec, many types can exist for the entries in various
  // configurations (i.e. differnet locales, landscape vs portrait, etc).
  virtual bool visit_type(android::ResTable_package* package,
                          android::ResTable_typeSpec* type_spec,
                          android::ResTable_type* type);
  // Visit a basic entry and its value.
  virtual bool visit_entry(android::ResTable_package* package,
                           android::ResTable_typeSpec* type_spec,
                           android::ResTable_type* type,
                           android::ResTable_entry* entry,
                           android::Res_value* value);
  // Visit a map entry. Values, if any will be dispatched to separate calls.
  virtual bool visit_map_entry(android::ResTable_package* package,
                               android::ResTable_typeSpec* type_spec,
                               android::ResTable_type* type,
                               android::ResTable_map_entry* entry);
  // Visit a map entry and its value (called many times for the same
  // android::ResTable_map_entry* pointer for the N ResTable_map* pointers).
  virtual bool visit_map_value(android::ResTable_package* package,
                               android::ResTable_typeSpec* type_spec,
                               android::ResTable_type* type,
                               android::ResTable_map_entry* entry,
                               android::ResTable_map* value);
  virtual ~ResourceTableVisitor() {}
  // Returns how far into the file this pointer is.
  long get_file_offset(const void* ptr) {
    auto delta = (const uint8_t*)ptr - (const uint8_t*)m_data;
    LOG_FATAL_IF(delta < 0, "Chunk %p is smaller than file start %p.", ptr,
                 m_data);
    LOG_FATAL_IF(delta >= (long)m_length,
                 "Chunk %p is beyond file end. Start = %p, Length = %zu", ptr,
                 m_data, m_length);
    return delta;
  }

 private:
  bool valid(const android::ResTable_package*);
  bool valid(const android::ResTable_typeSpec*);
  bool valid(const android::ResTable_type*);

  void* m_data;
  size_t m_length;
};

// A visitor that can find string pool references into multiple different pools!
// This, by default will traverse Res_value structs, which index into the global
// string pool, and Entries, which index into the key strings pool.
class StringPoolRefVisitor : public ResourceTableVisitor {
 public:
  // Meant to be overridden by sub classes if they need to access/edit key
  // strings.
  virtual bool visit_key_strings_ref(android::ResTable_package* package, android::ResStringPool_ref* ref);
  // Meant to be overridden by sub class if they need to access/edit global key
  // strings.
  virtual bool visit_global_strings_ref(android::Res_value* ref);
  // Meant to be overridden by sub class if they need to access/edit global key
  // strings. This method is for a style pointer into the global string pool.
  virtual bool visit_global_strings_ref(android::ResStringPool_ref* ref);
  using ResourceTableVisitor::visit_global_strings;
  bool visit_global_strings(android::ResStringPool_header* pool);
  using ResourceTableVisitor::visit_entry;
  bool visit_entry(android::ResTable_package* package,
                   android::ResTable_typeSpec* type_spec,
                   android::ResTable_type* type,
                   android::ResTable_entry* entry,
                   android::Res_value* value);
  using ResourceTableVisitor::visit_map_entry;
  bool visit_map_entry(android::ResTable_package* package,
                       android::ResTable_typeSpec* type_spec,
                       android::ResTable_type* type,
                       android::ResTable_map_entry* entry);
  using ResourceTableVisitor::visit_map_value;
  bool visit_map_value(android::ResTable_package* package,
                       android::ResTable_typeSpec* type_spec,
                       android::ResTable_type* type,
                       android::ResTable_map_entry* entry,
                       android::ResTable_map* value);
  virtual ~StringPoolRefVisitor() {}
};
} // namespace arsc
#endif
