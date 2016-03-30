/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <unordered_map>

#include "DexClass.h"
#include "Trace.h"
#include "Pass.h"

#include <locator.h>
using facebook::Locator;

typedef std::unordered_map<DexString*, uint32_t> dexstring_to_idx;
typedef std::unordered_map<DexType*, uint16_t> dextype_to_idx;
typedef std::unordered_map<DexProto*, uint32_t> dexproto_to_idx;
typedef std::unordered_map<DexField*, uint32_t> dexfield_to_idx;
typedef std::unordered_map<DexMethod*, uint32_t> dexmethod_to_idx;

using LocatorIndex = std::unordered_map<DexString*, Locator>;
LocatorIndex make_locator_index(const DexClassesVector& dexen);

class DexOutputIdx {
 private:
  dexstring_to_idx* m_string;
  dextype_to_idx* m_type;
  dexproto_to_idx* m_proto;
  dexfield_to_idx* m_field;
  dexmethod_to_idx* m_method;
  const uint8_t* m_base;

 public:
  DexOutputIdx(dexstring_to_idx* string,
               dextype_to_idx* type,
               dexproto_to_idx* proto,
               dexfield_to_idx* field,
               dexmethod_to_idx* method,
               const uint8_t* base) {
    m_string = string;
    m_type = type;
    m_proto = proto;
    m_field = field;
    m_method = method;
    m_base = base;
  }

  ~DexOutputIdx() {
    delete m_string;
    delete m_type;
    delete m_proto;
    delete m_field;
    delete m_method;
  }

  dextype_to_idx& type_to_idx() const { return *m_type; }
  dexproto_to_idx& proto_to_idx() const { return *m_proto; }
  dexfield_to_idx& field_to_idx() const { return *m_field; }
  dexmethod_to_idx& method_to_idx() const { return *m_method; }

  uint32_t stringidx(DexString* s) const { return m_string->at(s); }
  uint16_t typeidx(DexType* t) const { return m_type->at(t); }
  uint16_t protoidx(DexProto* p) const { return m_proto->at(p); }
  uint32_t fieldidx(DexField* f) const { return m_field->at(f); }
  uint32_t methodidx(DexMethod* m) const { return m_method->at(m); }

  int stringsize() const { return m_string->size(); }
  int typesize() const { return m_type->size(); }
  int protosize() const { return m_proto->size(); }
  int fieldsize() const { return m_field->size(); }
  int methodsize() const { return m_method->size(); }

  uint32_t get_offset(uint8_t* ptr) { return (uint32_t)(ptr - m_base); }

  uint32_t get_offset(uint32_t* ptr) { return get_offset((uint8_t*)ptr); }
};

struct dex_output_stats_t {
  int num_types = 0;
  int num_classes = 0;
  int num_methods = 0;
  int num_method_refs = 0;
  int num_fields = 0;
  int num_field_refs = 0;
  int num_strings = 0;
  int num_protos = 0;
  int num_static_values = 0;
  int num_annotations = 0;
  int num_type_lists = 0;
};

dex_output_stats_t&
  operator+=(dex_output_stats_t& lhs, const dex_output_stats_t& rhs);

dex_output_stats_t write_classes_to_dex(
  std::string filename,
  DexClasses* classes,
  LocatorIndex* locator_index /* nullable */,
  size_t dex_number,
  const char* method_mapping_filename);
