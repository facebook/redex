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

#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "Trace.h"
#include "Pass.h"
#include "ProguardMap.h"

#include <locator.h>
using facebook::Locator;

typedef std::unordered_map<DexString*, uint32_t> dexstring_to_idx;
typedef std::unordered_map<DexType*, uint16_t> dextype_to_idx;
typedef std::unordered_map<DexProto*, uint32_t> dexproto_to_idx;
typedef std::unordered_map<DexFieldRef*, uint32_t> dexfield_to_idx;
typedef std::unordered_map<DexMethodRef*, uint32_t> dexmethod_to_idx;

using LocatorIndex = std::unordered_map<DexString*, Locator>;
LocatorIndex make_locator_index(DexStoresVector& stores);

enum class SortMode {
  CLASS_ORDER,
  CLASS_STRINGS,
  CLINIT_FIRST,
  DEFAULT
};

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
  uint32_t fieldidx(DexFieldRef* f) const { return m_field->at(f); }
  uint32_t methodidx(DexMethodRef* m) const { return m_method->at(m); }

  size_t stringsize() const { return m_string->size(); }
  size_t typesize() const { return m_type->size(); }
  size_t protosize() const { return m_proto->size(); }
  size_t fieldsize() const { return m_field->size(); }
  size_t methodsize() const { return m_method->size(); }

  uint32_t get_offset(uint8_t* ptr) { return (uint32_t)(ptr - m_base); }

  uint32_t get_offset(uint32_t* ptr) { return get_offset((uint8_t*)ptr); }
};

dex_stats_t write_classes_to_dex(
  std::string filename,
  DexClasses* classes,
  LocatorIndex* locator_index /* nullable */,
  size_t dex_number,
  ConfigFiles& cfg,
  const Json::Value& json_cfg,
  PositionMapper* line_mapper);

typedef bool (*cmp_dstring)(const DexString*, const DexString*);
typedef bool (*cmp_dtype)(const DexType*, const DexType*);
typedef bool (*cmp_dproto)(const DexProto*, const DexProto*);
typedef bool (*cmp_dfield)(const DexFieldRef*, const DexFieldRef*);
typedef bool (*cmp_dmethod)(const DexMethodRef*, const DexMethodRef*);

/*
 * This API gathers all of the data referred to by a set of DexClasses in
 * preparation for emitting a dex file and provides the symbol tables in indexed
 * form for encoding.
 *
 * The gather algorithm implemented in gather_components() traverses the tree of
 * DexFoo objects rooted at each DexClass.  The individual gather methods,
 * gather_{strings,types,fields,methods}, (see Gatherable.h and DexClass.h) find
 * references to each type, respectively, that the object needs.
 *
 * Fields and methods need special consideration: those that are defined by a
 * DexClass need to emit more data (for example, methods must emit their code).
 * Fields and methods that are merely referenced by this DexClass (for example,
 * a call into the Android library) only need the types and strings necessary to
 * represent the reference.  To handle these divergent cases, gather_foo gathers
 * all of the data, while gather_foo_shallow gathers only what is needed for
 * references.
 *
 * Another subtlety is that gather_foo only follows fields of type foo.  For
 * example, DexField contains both a DexType (m_type) and a DexString (m_name).
 * Even though DexType also contains a string, DexField::gather_strings will
 * only gather m_name; it does not follow m_type to find more strings.  This
 * design simplifies the implentation of the gather methods since it breaks
 * cycles in the reference graph, but it makes finding a "complete" set more
 * involved.  To gather all strings, for instance, one must not only gather all
 * strings at the class level, but also gather strings for all types discovered
 * at the class level.
 */

class GatheredTypes {
 private:
  std::vector<DexString*> m_lstring;
  std::vector<DexType*> m_ltype;
  std::vector<DexFieldRef*> m_lfield;
  std::vector<DexMethodRef*> m_lmethod;
  DexClasses* m_classes;
  std::unordered_map<const DexString*, unsigned int> m_cls_load_strings;
  std::unordered_map<const DexString*, unsigned int> m_cls_strings;
  std::unordered_map<const DexMethod*, unsigned int> m_methods_in_cls_order;

  void gather_components();
  dexstring_to_idx* get_string_index(cmp_dstring cmp = compare_dexstrings);
  dextype_to_idx* get_type_index(cmp_dtype cmp = compare_dextypes);
  dexproto_to_idx* get_proto_index(cmp_dproto cmp = compare_dexprotos);
  dexfield_to_idx* get_field_index(cmp_dfield cmp = compare_dexfields);
  dexmethod_to_idx* get_method_index(cmp_dmethod cmp = compare_dexmethods);

  void build_cls_load_map();
  void build_cls_map();
  void build_method_map();

 public:
  GatheredTypes(DexClasses* classes);
  DexOutputIdx* get_dodx(const uint8_t* base);
  template <class T = decltype(compare_dexstrings)>
  std::vector<DexString*> get_dexstring_emitlist(T cmp = compare_dexstrings);
  std::vector<DexString*> get_cls_order_dexstring_emitlist();
  std::vector<DexString*> keep_cls_strings_together_emitlist();
  std::vector<DexMethod*> get_dexmethod_emitlist();

  void gather_class(int num);

  void sort_dexmethod_emitlist_default_order(std::vector<DexMethod*>& lmeth);
  void sort_dexmethod_emitlist_cls_order(std::vector<DexMethod*>& lmeth);
  void sort_dexmethod_emitlist_clinit_order(std::vector<DexMethod*>& lmeth);

  std::unordered_set<DexString*> index_type_names();
};

template <class T>
std::vector<DexString*> GatheredTypes::get_dexstring_emitlist(T cmp) {
  std::vector<DexString*> strlist(m_lstring);
  std::sort(strlist.begin(), strlist.end(), std::cref(cmp));
  return strlist;
}
