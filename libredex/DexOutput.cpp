/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <algorithm>
#include <memory>
#include <fcntl.h>
#include <sys/stat.h>
#include <list>
#include <stdlib.h>
#include <unordered_set>
#include <functional>
#include <exception>
#include <assert.h>

#include "Debug.h"
#include "DexClass.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "Pass.h"
#include "Resolver.h"
#include "Sha1.h"
#include "Trace.h"
#include "Transform.h"
#include "Walkers.h"
#include "WorkQueue.h"

/*
 * For adler32...
 */
#include <zlib.h>

typedef bool (*cmp_dstring)(const DexString*, const DexString*);
typedef bool (*cmp_dtype)(const DexType*, const DexType*);
typedef bool (*cmp_dproto)(const DexProto*, const DexProto*);
typedef bool (*cmp_dfield)(const DexField*, const DexField*);
typedef bool (*cmp_dmethod)(const DexMethod*, const DexMethod*);

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
  std::vector<DexField*> m_lfield;
  std::vector<DexMethod*> m_lmethod;
  DexClasses* m_classes;
  std::map<const DexString*, unsigned int> m_cls_load_strings;
  std::map<const DexString*, unsigned int> m_cls_strings;
  std::map<const DexMethod*, unsigned int> m_methods_in_cls_order;

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
  template <class T = decltype(compare_dexmethods)>
  std::vector<DexMethod*> get_dexmethod_emitlist(T cmp = compare_dexmethods);
  std::vector<DexMethod*> get_cls_order_dexmethod_emitlist();
  void gather_class(int num);

  std::unordered_set<DexString*> index_type_names();
};

template<class T, class U>
class CustomSort {
  private:
    std::map<const T*, unsigned int> m_map;
    U m_cmp;

  public:
    CustomSort(std::map<const T*, unsigned int> input_map, U cmp) {
      m_map = input_map;
      m_cmp = cmp;
    }

    bool operator()(const T* a, const T* b) const {
      bool a_in = m_map.count(a);
      bool b_in = m_map.count(b);
      if (!a_in && !b_in) {
        return m_cmp(a,b);
      } else if (a_in && b_in) {
        return (m_map.at(a) < m_map.at(b));
      } else if (a_in) {
        return true;
      } else {
        return false;
      }
    }
};

GatheredTypes::GatheredTypes(DexClasses* classes)
  : m_classes(classes)
{
  // ensure that the string id table contains the empty string, which is used
  // for the DexPosition mapping
  m_lstring.push_back(DexString::make_string(""));

  // build maps for the different custom sorting options
  build_cls_load_map();
  build_cls_map();
  build_method_map();

  gather_components();
}

std::unordered_set<DexString*> GatheredTypes::index_type_names() {
  std::unordered_set<DexString*> type_names;
  for (auto it = m_ltype.begin(); it != m_ltype.end(); ++it) {
    type_names.insert((*it)->get_name());
  }
  return type_names;
}

template <class T>
std::vector<DexString*> GatheredTypes::get_dexstring_emitlist(T cmp) {
  std::vector<DexString*> strlist(m_lstring);
  std::sort(strlist.begin(), strlist.end(), std::cref(cmp));
  return strlist;
}

std::vector<DexString*> GatheredTypes::get_cls_order_dexstring_emitlist() {
  return get_dexstring_emitlist(CustomSort<DexString, cmp_dstring>(
        m_cls_load_strings,
        compare_dexstrings));
}

std::vector<DexString*> GatheredTypes::keep_cls_strings_together_emitlist() {
  return get_dexstring_emitlist(CustomSort<DexString, cmp_dstring>(
        m_cls_strings,
        compare_dexstrings));
}

template <class T>
std::vector<DexMethod*> GatheredTypes::get_dexmethod_emitlist(T cmp) {
  std::vector<DexMethod*> methlist;
  for (auto cls : *m_classes) {
    auto const& dmethods = cls->get_dmethods();
    auto const& vmethods = cls->get_vmethods();
    methlist.insert(methlist.end(), dmethods.begin(), dmethods.end());
    methlist.insert(methlist.end(), vmethods.begin(), vmethods.end());
  }
  std::sort(methlist.begin(), methlist.end(), std::cref(cmp));
  return methlist;
}

std::vector<DexMethod*> GatheredTypes::get_cls_order_dexmethod_emitlist() {
  return get_dexmethod_emitlist(CustomSort<DexMethod, cmp_dmethod>(
        m_methods_in_cls_order,
        compare_dexmethods));
}

DexOutputIdx* GatheredTypes::get_dodx(const uint8_t* base) {
  /*
   * These are symbol table indices.  Symbols which are used
   * should be bunched together.  We will pass a different
   * sort routine here to optimize.  Doing so does violate
   * the dex spec.  However, that aspect of the spec is only
   * used in certain scenarios.  For strings, types, and protos
   * that aspect of the spec has no runtime dependency.  For
   * methods and fields, only dexes with annotations have a
   * dependency on ordering.
   */
  dexstring_to_idx* string = get_string_index();
  dextype_to_idx* type = get_type_index();
  dexproto_to_idx* proto = get_proto_index();
  dexfield_to_idx* field = get_field_index();
  dexmethod_to_idx* method = get_method_index();
  return new DexOutputIdx(string, type, proto, field, method, base);
}

dexstring_to_idx* GatheredTypes::get_string_index(cmp_dstring cmp) {
  std::sort(m_lstring.begin(), m_lstring.end(), cmp);
  dexstring_to_idx* sidx = new dexstring_to_idx();
  uint32_t idx = 0;
  for (auto it = m_lstring.begin(); it != m_lstring.end(); it++) {
    sidx->insert(std::make_pair(*it, idx++));
  }
  return sidx;
}

dextype_to_idx* GatheredTypes::get_type_index(cmp_dtype cmp) {
  std::sort(m_ltype.begin(), m_ltype.end(), cmp);
  dextype_to_idx* sidx = new dextype_to_idx();
  uint32_t idx = 0;
  for (auto it = m_ltype.begin(); it != m_ltype.end(); it++) {
    sidx->insert(std::make_pair(*it, idx++));
  }
  return sidx;
}

dexfield_to_idx* GatheredTypes::get_field_index(cmp_dfield cmp) {
  std::sort(m_lfield.begin(), m_lfield.end(), cmp);
  dexfield_to_idx* sidx = new dexfield_to_idx();
  uint32_t idx = 0;
  for (auto it = m_lfield.begin(); it != m_lfield.end(); it++) {
    sidx->insert(std::make_pair(*it, idx++));
  }
  return sidx;
}

dexmethod_to_idx* GatheredTypes::get_method_index(cmp_dmethod cmp) {
  std::sort(m_lmethod.begin(), m_lmethod.end(), cmp);
  dexmethod_to_idx* sidx = new dexmethod_to_idx();
  uint32_t idx = 0;
  for (auto it = m_lmethod.begin(); it != m_lmethod.end(); it++) {
    sidx->insert(std::make_pair(*it, idx++));
  }
  return sidx;
}

dexproto_to_idx* GatheredTypes::get_proto_index(cmp_dproto cmp) {
  std::list<DexProto*> protos;
  for (auto const& m : m_lmethod) {
    protos.push_back(m->get_proto());
  }
  protos.sort();
  protos.unique();
  protos.sort(cmp);
  dexproto_to_idx* sidx = new dexproto_to_idx();
  uint32_t idx = 0;
  for (auto const& proto : protos) {
    sidx->insert(std::make_pair(proto, idx++));
  }
  return sidx;
}

void GatheredTypes::build_cls_load_map() {
  unsigned int index = 0;
  int type_strings = 0;
  int init_strings = 0;
  int total_strings = 0;
  for (const auto& cls : *m_classes) {
    // gather type first, assuming class load will check all components of a class first
    std::vector<DexType*> cls_types;
    cls->gather_types(cls_types);
    for (const auto& t : cls_types) {
      if (!m_cls_load_strings.count(t->get_name())) {
        m_cls_load_strings[t->get_name()] = index;
        index++;
        type_strings++;
      }
    }
    // now add in any strings found in <clinit>
    // since they are likely to be accessed during class load
    for (const auto& m: cls->get_dmethods()) {
      if (is_clinit(m)) {
        std::vector<DexString*> method_strings;
        m->gather_strings(method_strings);
        for (const auto& s : method_strings) {
          if (!m_cls_load_strings.count(s)) {
            m_cls_load_strings[s] = index;
            index++;
            init_strings++;
          }
        }
      }
    }
  }
  total_strings += type_strings + init_strings;
  for (const auto& cls : *m_classes) {
    // now add all other strings in class order. This way we get some
    // locality if a random class in a dex is loaded and then executes some methods
    std::vector<const DexClass*> v;
    v.push_back(cls);
    walk_methods(v, [&](DexMethod* m) {
      std::vector<DexString*> method_strings;
      m->gather_strings(method_strings);
      for (const auto& s : method_strings) {
        if (!m_cls_load_strings.count(s)) {
          m_cls_load_strings[s] = index;
          index++;
          total_strings++;
        }
      }
    });
  }
  TRACE(CUSTOMSORT, 1, "found %d strings from types, %d from strings in init methods, %d total strings\n",
      type_strings,
      init_strings,
      total_strings);
}

void GatheredTypes::build_cls_map() {
  int index = 0;
  for (const auto& cls : *m_classes) {
      if (!m_cls_strings.count(cls->get_name())) {
        m_cls_strings[cls->get_name()] = index++;
      }
  }
}

void GatheredTypes::build_method_map() {
  auto index = 0;
  for (const auto& cls : *m_classes) {
    for (const auto& m : cls->get_dmethods()) {
      if (!m_methods_in_cls_order.count(m)) {
        m_methods_in_cls_order[m] = index;
      }
    }
    for (const auto& m : cls->get_vmethods()) {
      if (!m_methods_in_cls_order.count(m)) {
        m_methods_in_cls_order[m] = index;
      }
    }
    index++;
  }
}

void GatheredTypes::gather_components() {
  // Gather references reachable from each class.
  for (auto const& cls : *m_classes) {
    cls->gather_strings(m_lstring);
    cls->gather_types(m_ltype);
    cls->gather_fields(m_lfield);
    cls->gather_methods(m_lmethod);
  }

  // Remove duplicates to speed up the later loops.
  sort_unique(m_lstring);
  sort_unique(m_ltype);

  // Gather types and strings needed for field and method refs.
  sort_unique(m_lmethod);
  for (auto meth : m_lmethod) {
    meth->gather_types_shallow(m_ltype);
    meth->gather_strings_shallow(m_lstring);
  }

  sort_unique(m_lfield);
  for (auto field : m_lfield) {
    field->gather_types_shallow(m_ltype);
    field->gather_strings_shallow(m_lstring);
  }

  // Gather strings needed for each type.
  sort_unique(m_ltype);
  for (auto type : m_ltype) {
    if (type) m_lstring.push_back(type->get_name());
  }

  sort_unique(m_lstring);
}

constexpr uint32_t k_max_dex_size = 16 * 1024 * 1024;
typedef std::map<DexAnnotation*, uint32_t> annomap_t;
typedef std::map<DexAnnotationSet*, uint32_t> asetmap_t;
typedef std::map<ParamAnnotations*, uint32_t> xrefmap_t;
typedef std::map<DexAnnotationDirectory*, uint32_t> adirmap_t;

class DexOutput {
public:
  dex_output_stats_t m_stats;
 private:
  DexClasses* m_classes;
  DexOutputIdx* dodx;
  GatheredTypes* m_gtypes;
  uint8_t* m_output;
  uint32_t m_offset;
  const char* m_filename;
  size_t m_dex_number;
  PositionMapper* m_pos_mapper;
  std::string m_method_mapping_filename;
  std::string m_class_mapping_filename;
  std::map<DexTypeList*, uint32_t> m_tl_emit_offsets;
  std::vector<std::pair<DexCode*, dex_code_item*>> m_code_item_emits;
  std::map<DexClass*, uint32_t> m_cdi_offsets;
  std::map<DexClass*, uint32_t> m_static_values;
  dex_header hdr;
  std::vector<dex_map_item> m_map_items;
  LocatorIndex* m_locator_index;
  ConfigFiles& m_config_files;

  void insert_map_item(uint16_t typeidx, uint32_t size, uint32_t offset);
  void generate_string_data(SortMode mode = SortMode::DEFAULT);
  void generate_type_data();
  void generate_proto_data();
  void generate_field_data();
  void generate_method_data();
  void generate_class_data();
  void generate_class_data_items();
  void generate_code_items(SortMode mode = SortMode::DEFAULT);
  void generate_static_values();
  void unique_annotations(annomap_t& annomap,
                          std::vector<DexAnnotation*>& annolist);
  void unique_asets(annomap_t& annomap,
                    asetmap_t& asetmap,
                    std::vector<DexAnnotationSet*>& asetlist);
  void unique_xrefs(asetmap_t& asetmap,
                    xrefmap_t& xrefmap,
                    std::vector<ParamAnnotations*>& xreflist);
  void unique_adirs(asetmap_t& asetmap,
                    xrefmap_t& xrefmap,
                    adirmap_t& adirmap,
                    std::list<DexAnnotationDirectory*>& adirlist);
  void generate_annotations();
  void generate_debug_items();
  void generate_typelist_data();
  void generate_map();
  void finalize_header();
  void init_header_offsets();
  void write_symbol_files();
  void align_output() { m_offset = (m_offset + 3) & ~3; }
  void emit_locator(Locator locator);
  std::unique_ptr<Locator> locator_for_descriptor(
    const std::unordered_set<DexString*>& type_names,
    DexString* descriptor);

 public:
  DexOutput(
    const char* path,
    DexClasses* classes,
    LocatorIndex* locator_index,
    size_t dex_number,
    ConfigFiles& config_files,
    PositionMapper* pos_mapper,
    std::string method_mapping_path,
    std::string class_mapping_path);
  ~DexOutput();
  void prepare(SortMode string_mode, SortMode code_mode);
  void write();
};

DexOutput::DexOutput(
  const char* path,
  DexClasses* classes,
  LocatorIndex* locator_index,
  size_t dex_number,
  ConfigFiles& config_files,
  PositionMapper* pos_mapper,
  std::string method_mapping_path,
  std::string class_mapping_path)
    : m_config_files(config_files)
{
  m_classes = classes;
  m_output = (uint8_t*)malloc(k_max_dex_size);
  memset(m_output, 0, k_max_dex_size);
  m_offset = 0;
  m_gtypes = new GatheredTypes(classes);
  dodx = m_gtypes->get_dodx(m_output);
  m_filename = path;
  m_pos_mapper = pos_mapper,
  m_method_mapping_filename = method_mapping_path;
  m_class_mapping_filename = class_mapping_path;
  m_dex_number = dex_number;
  m_locator_index = locator_index;
}

DexOutput::~DexOutput() {
  delete m_gtypes;
  delete dodx;
  free(m_output);
}

void DexOutput::insert_map_item(uint16_t maptype,
                                uint32_t size,
                                uint32_t offset) {
  if (size == 0) return;
  dex_map_item item{};
  item.type = maptype;
  item.size = size;
  item.offset = offset;
  m_map_items.emplace_back(item);
}

void DexOutput::emit_locator(Locator locator) {
  char buf[Locator::encoded_max];
  locator.encode(buf);
  size_t locator_length = strlen(buf); // ASCII-only
  write_uleb128(m_output + m_offset, (uint32_t) locator_length);
  m_offset += uleb128_encoding_size((uint32_t) locator_length);
  memcpy(m_output + m_offset, buf, locator_length + 1);
  m_offset += locator_length + 1;
}

std::unique_ptr<Locator>
DexOutput::locator_for_descriptor(
  const std::unordered_set<DexString*>& type_names,
  DexString* descriptor)
{
  LocatorIndex* locator_index = m_locator_index;
  if (locator_index != nullptr) {
    auto locator_it = locator_index->find(descriptor);
    if (locator_it != locator_index->end()) {
      // This string is the name of a type we define in one of our
      // dex files.
      return std::unique_ptr<Locator>(new Locator(locator_it->second));
    }

    if (type_names.count(descriptor)) {
      // If we're emitting an array name, see whether the element
      // type is one of ours; if so, emit a locator for that type.
      const char* s = descriptor->c_str();
      if (s[0] == '[') {
        while (*s == '[') ++s;
        DexString* elementDescriptor = DexString::get_string(s);
        if (elementDescriptor != nullptr) {
          locator_it = locator_index->find(elementDescriptor);
          if (locator_it != locator_index->end()) {
            return std::unique_ptr<Locator>(new Locator(locator_it->second));
          }
        }
      }

      // We have the name of a type, but it's not a type we define.
      // Emit the special locator that indicates we should look in the
      // system classloader.
      return std::unique_ptr<Locator>(new Locator(Locator::make(0, 0)));
    }
  }

  return nullptr;
}

void DexOutput::generate_string_data(SortMode mode) {
  /*
   * This is a index to position within the string data.  There
   * is no specific ordering specified here for the dex spec.
   * The optimized sort here would be different than the one
   * for the symbol table.  The symbol table should be packed
   * for strings that are used by the opcode const-string.  Whereas
   * this should be ordered by access for page-cache efficiency.
   */
  std::vector<DexString*> string_order;
  if (mode == CLASS_ORDER) {
    TRACE(CUSTOMSORT, 2, "using class order for string pool sorting\n");
    string_order = m_gtypes->get_cls_order_dexstring_emitlist();
  } else if (mode == CLASS_STRINGS) {
    TRACE(CUSTOMSORT, 2, "using class names pack for string pool sorting\n");
    string_order = m_gtypes->keep_cls_strings_together_emitlist();
  } else {
    TRACE(CUSTOMSORT, 2, "using default string pool sorting\n");
    string_order = m_gtypes->get_dexstring_emitlist();
  }
  dex_string_id* stringids = (dex_string_id*)(m_output + hdr.string_ids_off);

  std::unordered_set<DexString*> type_names = m_gtypes->index_type_names();
  unsigned locator_size = 0;

  // If we're generating locator strings, we need to include them in
  // the total count of strings in this section.
  size_t nrstr = string_order.size();
  for (DexString* str : string_order) {
    if (locator_for_descriptor(type_names, str)) {
      nrstr += 1;
    }
  }

  insert_map_item(TYPE_STRING_DATA_ITEM, (uint32_t) nrstr, m_offset);
  for (DexString* str : string_order) {
    // Emit lookup acceleration string if requested
    std::unique_ptr<Locator> locator = locator_for_descriptor(type_names, str);
    if (locator) {
      unsigned orig_offset = m_offset;
      emit_locator(*locator);
      locator_size += m_offset - orig_offset;
    }

    // Emit the string itself
    uint32_t idx = dodx->stringidx(str);
    TRACE(CUSTOMSORT, 3, "str emit %s\n", SHOW(str));
    stringids[idx].offset = m_offset;
    str->encode(m_output + m_offset);
    m_offset += str->get_entry_size();
    m_stats.num_strings++;
  }

  if (m_locator_index != nullptr) {
    TRACE(LOC, 1, "Used %u bytes for locator strings\n", locator_size);
  }
}

void DexOutput::generate_type_data() {
  dex_type_id* typeids = (dex_type_id*)(m_output + hdr.type_ids_off);
  for (auto& p : dodx->type_to_idx()) {
    auto t = p.first;
    auto idx = p.second;
    typeids[idx].string_idx = dodx->stringidx(t->get_name());
    m_stats.num_types++;
  }
}

void DexOutput::generate_typelist_data() {
  std::vector<DexTypeList*> typel;
  for (auto& it : dodx->proto_to_idx()) {
    auto proto = it.first;
    typel.push_back(proto->get_args());
  }
  for (uint32_t i = 0; i < hdr.class_defs_size; i++) {
    DexClass* clz = m_classes->get(i);
    typel.push_back(clz->get_interfaces());
  }
  sort_unique(typel, compare_dextypelists);
  align_output();
  uint32_t tl_start = m_offset;
  size_t num_tls = 0;
  for (DexTypeList* tl : typel) {
    if (tl->get_type_list().size() == 0) {
      m_tl_emit_offsets[tl] = 0;
      continue;
    }
    ++num_tls;
    align_output();
    m_tl_emit_offsets[tl] = m_offset;
    int size = tl->encode(dodx, (uint32_t*)(m_output + m_offset));
    m_offset += size;
    m_stats.num_type_lists++;
  }
  insert_map_item(TYPE_TYPE_LIST, (uint32_t) num_tls, tl_start);
}

void DexOutput::generate_proto_data() {
  auto protoids = (dex_proto_id*)(m_output + hdr.proto_ids_off);

  for (auto& it : dodx->proto_to_idx()) {
    auto proto = it.first;
    auto idx = it.second;
    protoids[idx].shortyidx = dodx->stringidx(proto->get_shorty());
    protoids[idx].rtypeidx = dodx->typeidx(proto->get_rtype());
    protoids[idx].param_off = m_tl_emit_offsets.at(proto->get_args());
    m_stats.num_protos++;
  }
}

void DexOutput::generate_field_data() {
  auto fieldids = (dex_field_id*)(m_output + hdr.field_ids_off);
  for (auto& it : dodx->field_to_idx()) {
    auto field = it.first;
    auto idx = it.second;
    fieldids[idx].classidx = dodx->typeidx(field->get_class());
    fieldids[idx].typeidx = dodx->typeidx(field->get_type());
    fieldids[idx].nameidx = dodx->stringidx(field->get_name());
    if (field->is_concrete()) m_stats.num_fields++;
    m_stats.num_field_refs++;

  }
}

void DexOutput::generate_method_data() {
  constexpr size_t kMaxMethodRefs = 64 * 1024;
  constexpr size_t kMaxFieldRefs = 64 * 1024;
  always_assert_log(
      dodx->method_to_idx().size() <= kMaxMethodRefs,
      "Trying to encode too many method refs in a dex: %lu (limit: %lu)",
      dodx->method_to_idx().size(),
      kMaxMethodRefs);
  always_assert_log(
      dodx->field_to_idx().size() <= kMaxFieldRefs,
      "Trying to encode too many field refs in a dex: %lu (limit: %lu)",
      dodx->field_to_idx().size(),
      kMaxFieldRefs);
  auto methodids = (dex_method_id*)(m_output + hdr.method_ids_off);
  for (auto& it : dodx->method_to_idx()) {
    auto method = it.first;
    auto idx = it.second;
    methodids[idx].classidx = dodx->typeidx(method->get_class());
    methodids[idx].protoidx = dodx->protoidx(method->get_proto());
    methodids[idx].nameidx = dodx->stringidx(method->get_name());
    if (method->is_concrete()) m_stats.num_methods++;
    m_stats.num_method_refs++;
  }
}

void DexOutput::generate_class_data() {
  dex_class_def* cdefs = (dex_class_def*)(m_output + hdr.class_defs_off);
  for (uint32_t i = 0; i < hdr.class_defs_size; i++) {
    m_stats.num_classes++;
    DexClass* clz = m_classes->get(i);
    cdefs[i].typeidx = dodx->typeidx(clz->get_type());
    cdefs[i].access_flags = clz->get_access();
    cdefs[i].super_idx = dodx->typeidx(clz->get_super_class());
    cdefs[i].interfaces_off = 0;
    cdefs[i].annotations_off = 0;
    cdefs[i].interfaces_off = m_tl_emit_offsets[clz->get_interfaces()];
    auto source_file = m_pos_mapper->get_source_file(clz);
    if (source_file != nullptr) {
      cdefs[i].source_file_idx = dodx->stringidx(source_file);
    } else {
      cdefs[i].source_file_idx = DEX_NO_INDEX;
    }
    if (m_cdi_offsets.count(clz)) {
      cdefs[i].class_data_offset = m_cdi_offsets[clz];
    } else {
      cdefs[i].class_data_offset = 0;
      always_assert_log(
          clz->get_dmethods().size() == 0 &&
          clz->get_vmethods().size() == 0 &&
          clz->get_ifields().size() == 0 &&
          clz->get_sfields().size() == 0,
          "DexClass %s has member but no class data!\n",
          SHOW(clz->get_type()));
    }
    if (m_static_values.count(clz)) {
      cdefs[i].static_values_off = m_static_values[clz];
    } else {
      cdefs[i].static_values_off = 0;
    }
  }
}

void DexOutput::generate_class_data_items() {
  /*
   * First generate a dexcode_to_offset needed for the encoding
   * of class_data_items
   */
  dexcode_to_offset dco;
  uint32_t cdi_start = m_offset;
  for (auto& it : m_code_item_emits) {
    uint32_t offset = (uint32_t) (((uint8_t*)it.second) - m_output);
    dco[it.first] = offset;
  }
  for (uint32_t i = 0; i < hdr.class_defs_size; i++) {
    DexClass* clz = m_classes->get(i);
    if (!clz->has_class_data()) continue;
    /* No alignment constraints for this data */
    int size = clz->encode(dodx, dco, m_output + m_offset);
    m_cdi_offsets[clz] = m_offset;
    m_offset += size;
  }
  insert_map_item(TYPE_CLASS_DATA_ITEM, (uint32_t) m_cdi_offsets.size(), cdi_start);
}

void DexOutput::generate_code_items(SortMode mode) {
  /*
   * Optimization note:  We should pass a sort routine to the
   * emitlist to optimize pagecache efficiency.
   */
  align_output();
  uint32_t ci_start = m_offset;

  std::vector<DexMethod*> lmeth;
  if (mode == CLASS_ORDER) {
    TRACE(CUSTOMSORT, 2, "using class order for bytecode sorting\n");
    lmeth = m_gtypes->get_cls_order_dexmethod_emitlist();
  } else {
    TRACE(CUSTOMSORT, 2, "using default bytecode sorting\n");
    lmeth = m_gtypes->get_dexmethod_emitlist();
  }
  for (DexMethod* meth : lmeth) {
    if (meth->get_access() & (DEX_ACCESS_ABSTRACT | DEX_ACCESS_NATIVE)) {
      // There is no code item for ABSTRACT or NATIVE methods.
      continue;
    }
    TRACE(CUSTOMSORT, 3, "method emit %s %s\n", SHOW(meth->get_class()), SHOW(meth));
    std::unique_ptr<DexCode>& code = meth->get_code();
    always_assert_log(
        meth->is_concrete() && code != nullptr,
        "Undefined method in generate_code_items()\n\t prototype: %s\n",
        show_short(meth).c_str());
    align_output();
    int size = code->encode(dodx, (uint32_t*)(m_output + m_offset));
    m_code_item_emits.emplace_back(code.get(),
                                   (dex_code_item*)(m_output + m_offset));
    m_offset += size;
  }
  insert_map_item(TYPE_CODE_ITEM, (uint32_t) m_code_item_emits.size(), ci_start);
}

void DexOutput::generate_static_values() {
  uint32_t sv_start = m_offset;
  std::unordered_map<DexEncodedValueArray,
                     uint32_t,
                     boost::hash<DexEncodedValueArray>>
      enc_arrays;
  for (uint32_t i = 0; i < hdr.class_defs_size; i++) {
    DexClass* clz = m_classes->get(i);
    std::unique_ptr<DexEncodedValueArray> deva(clz->get_static_values());
    if (!deva) continue;
    if (enc_arrays.count(*deva)) {
      m_static_values[clz] = enc_arrays.at(*deva);
    } else {
      uint8_t* output = m_output + m_offset;
      uint8_t* outputsv = output;
      /* No alignment requirements */
      deva->encode(dodx, output);
      enc_arrays.emplace(std::move(*deva.release()), m_offset);
      m_static_values[clz] = m_offset;
      m_offset += output - outputsv;
      m_stats.num_static_values++;
    }
  }
  if (m_static_values.size()) {
    insert_map_item(TYPE_ENCODED_ARRAY_ITEM, (uint32_t) enc_arrays.size(), sv_start);
  }
}

static bool annotation_cmp(const DexAnnotationDirectory* a,
                           const DexAnnotationDirectory* b) {
  return (a->viz_score() < b->viz_score());
}

void DexOutput::unique_annotations(annomap_t& annomap,
                                   std::vector<DexAnnotation*>& annolist) {
  int annocnt = 0;
  uint32_t mentry_offset = m_offset;
  std::map<std::vector<uint8_t>, uint32_t> annotation_byte_offsets;
  for (auto anno : annolist) {
    if (annomap.count(anno)) continue;
    std::vector<uint8_t> annotation_bytes;
    anno->vencode(dodx, annotation_bytes);
    if (annotation_byte_offsets.count(annotation_bytes)) {
      annomap[anno] = annotation_byte_offsets[annotation_bytes];
      continue;
    }
    /* Insert new annotation in tracking structs */
    annotation_byte_offsets[annotation_bytes] = m_offset;
    annomap[anno] = m_offset;
    /* Not a dupe, encode... */
    uint8_t* annoout = (uint8_t*)(m_output + m_offset);
    memcpy(annoout, &annotation_bytes[0], annotation_bytes.size());
    m_offset += annotation_bytes.size();
    annocnt++;
  }
  if (annocnt) {
    insert_map_item(TYPE_ANNOTATION_ITEM, annocnt, mentry_offset);
  }
  m_stats.num_annotations += annocnt;
}

void DexOutput::unique_asets(annomap_t& annomap,
                             asetmap_t& asetmap,
                             std::vector<DexAnnotationSet*>& asetlist) {
  int asetcnt = 0;
  uint32_t mentry_offset = m_offset;
  std::map<std::vector<uint32_t>, uint32_t> aset_offsets;
  for (auto aset : asetlist) {
    if (asetmap.count(aset)) continue;
    std::vector<uint32_t> aset_bytes;
    aset->vencode(dodx, aset_bytes, annomap);
    if (aset_offsets.count(aset_bytes)) {
      asetmap[aset] = aset_offsets[aset_bytes];
      continue;
    }
    /* Insert new aset in tracking structs */
    aset_offsets[aset_bytes] = m_offset;
    asetmap[aset] = m_offset;
    /* Not a dupe, encode... */
    uint8_t* asetout = (uint8_t*)(m_output + m_offset);
    memcpy(asetout, &aset_bytes[0], aset_bytes.size() * sizeof(uint32_t));
    m_offset += aset_bytes.size() * sizeof(uint32_t);
    asetcnt++;
  }
  if (asetcnt) {
    insert_map_item(TYPE_ANNOTATION_SET_ITEM, asetcnt, mentry_offset);
  }
}

void DexOutput::unique_xrefs(asetmap_t& asetmap,
                             xrefmap_t& xrefmap,
                             std::vector<ParamAnnotations*>& xreflist) {
  int xrefcnt = 0;
  uint32_t mentry_offset = m_offset;
  std::map<std::vector<uint32_t>, uint32_t> xref_offsets;
  for (auto xref : xreflist) {
    if (xrefmap.count(xref)) continue;
    std::vector<uint32_t> xref_bytes;
    xref_bytes.push_back((unsigned int) xref->size());
    for (auto param : *xref) {
      DexAnnotationSet* das = param.second;
      always_assert_log(asetmap.count(das) != 0,
                        "Uninitialized aset %p '%s'", das, SHOW(das));
      xref_bytes.push_back(asetmap[das]);
    }
    if (xref_offsets.count(xref_bytes)) {
      xrefmap[xref] = xref_offsets[xref_bytes];
      continue;
    }
    /* Insert new xref in tracking structs */
    xref_offsets[xref_bytes] = m_offset;
    xrefmap[xref] = m_offset;
    /* Not a dupe, encode... */
    uint8_t* xrefout = (uint8_t*)(m_output + m_offset);
    memcpy(xrefout, &xref_bytes[0], xref_bytes.size() * sizeof(uint32_t));
    m_offset += xref_bytes.size() * sizeof(uint32_t);
    xrefcnt++;
  }
  if (xrefcnt) {
    insert_map_item(TYPE_ANNOTATION_SET_REF_LIST, xrefcnt, mentry_offset);
  }
}

void DexOutput::unique_adirs(asetmap_t& asetmap,
                             xrefmap_t& xrefmap,
                             adirmap_t& adirmap,
                             std::list<DexAnnotationDirectory*>& adirlist) {
  int adircnt = 0;
  uint32_t mentry_offset = m_offset;
  std::map<std::vector<uint32_t>, uint32_t> adir_offsets;
  for (auto adir : adirlist) {
    if (adirmap.count(adir)) continue;
    std::vector<uint32_t> adir_bytes;
    adir->vencode(dodx, adir_bytes, xrefmap, asetmap);
    if (adir_offsets.count(adir_bytes)) {
      adirmap[adir] = adir_offsets[adir_bytes];
      continue;
    }
    /* Insert new adir in tracking structs */
    adir_offsets[adir_bytes] = m_offset;
    adirmap[adir] = m_offset;
    /* Not a dupe, encode... */
    uint8_t* adirout = (uint8_t*)(m_output + m_offset);
    memcpy(adirout, &adir_bytes[0], adir_bytes.size() * sizeof(uint32_t));
    m_offset += adir_bytes.size() * sizeof(uint32_t);
    adircnt++;
  }
  if (adircnt) {
    insert_map_item(TYPE_ANNOTATIONS_DIR_ITEM, adircnt, mentry_offset);
  }
}

void DexOutput::generate_annotations() {
  /*
   * There are five phases to generating annotations:
   * 1) Emit annotations
   * 2) Emit annotation_sets
   * 3) Emit annotation xref lists for method params
   * 4) Emit annotation_directories
   * 5) Attach annotation_directories to the classdefs
   */
  std::list<DexAnnotationDirectory*> lad;
  int xrefsize = 0;
  int annodirsize = 0;
  int xrefcnt = 0;
  std::map<DexAnnotationDirectory*, int> ad_to_classnum;
  annomap_t annomap;
  asetmap_t asetmap;
  xrefmap_t xrefmap;
  adirmap_t adirmap;

  for (uint32_t i = 0; i < hdr.class_defs_size; i++) {
    DexClass* clz = m_classes->get(i);
    DexAnnotationDirectory* ad = clz->get_annotation_directory();
    if (ad) {
      xrefsize += ad->xref_size();
      annodirsize += ad->annodir_size();
      xrefcnt += ad->xref_count();
      lad.push_back(ad);
      ad_to_classnum[ad] = i;
    }
  }
  lad.sort(annotation_cmp);
  std::vector<DexAnnotation*> annolist;
  std::vector<DexAnnotationSet*> asetlist;
  std::vector<ParamAnnotations*> xreflist;
  for (auto ad : lad) {
    ad->gather_asets(asetlist);
    ad->gather_annotations(annolist);
    ad->gather_xrefs(xreflist);
  }
  unique_annotations(annomap, annolist);
  align_output();
  unique_asets(annomap, asetmap, asetlist);
  unique_xrefs(asetmap, xrefmap, xreflist);
  unique_adirs(asetmap, xrefmap, adirmap, lad);
  for (auto ad : lad) {
    int class_num = ad_to_classnum[ad];
    dex_class_def* cdefs = (dex_class_def*)(m_output + hdr.class_defs_off);
    cdefs[class_num].annotations_off = adirmap[ad];
    delete ad;
  }
}

void DexOutput::generate_debug_items() {
  uint32_t dbg_start = m_offset;
  int dbgcount = 0;
  for (auto& it : m_code_item_emits) {
    DexCode* dc = it.first;
    dex_code_item* dci = it.second;
    auto& dbg = dc->get_debug_item();
    if (dbg == nullptr) continue;
    dbgcount++;
    // No align requirement for debug items.
    int size = dbg->encode(dodx, m_pos_mapper, m_output + m_offset);
    dci->debug_info_off = m_offset;
    m_offset += size;
  }
  insert_map_item(TYPE_DEBUG_INFO_ITEM, dbgcount, dbg_start);
}

void DexOutput::generate_map() {
  align_output();
  uint32_t* mapout = (uint32_t*)(m_output + m_offset);
  hdr.map_off = m_offset;
  insert_map_item(TYPE_MAP_LIST, 1, m_offset);
  *mapout = (uint32_t) m_map_items.size();
  dex_map_item* map = (dex_map_item*)(mapout + 1);
  for (auto const& mit : m_map_items) {
    *map++ = mit;
  }
  m_offset += ((uint8_t*)map) - ((uint8_t*)mapout);
}

/**
 * When things move around in redex, we might find ourselves in a situation
 * where a regular OPCODE_CONST_STRING is now referring to a jumbo string,
 * or vice versea. This fixup ensures that all const string opcodes agree
 * with the jumbo-ness of their stridx.
 */
static void fix_method_jumbos(DexMethod* method, const DexOutputIdx* dodx) {
  auto& code = method->get_code();
  if (!code) return; // nothing to do for native methods

  auto& insts = code->get_instructions();
  std::unordered_set<DexOpcodeString*> jumbo_mismatches;
  for (auto inst : insts) {
    auto op = inst->opcode();
    if (op != OPCODE_CONST_STRING && op != OPCODE_CONST_STRING_JUMBO) continue;

    auto str_opcode = static_cast<DexOpcodeString*>(inst);
    auto str = str_opcode->get_string();
    uint32_t stridx = dodx->stringidx(str);
    bool jumbo = ((stridx >> 16) != 0);

    if (jumbo && op == OPCODE_CONST_STRING) {
      jumbo_mismatches.emplace(str_opcode);
    } else if (!jumbo && op == OPCODE_CONST_STRING_JUMBO) {
      jumbo_mismatches.emplace(str_opcode);
    }
  }
  if (jumbo_mismatches.empty()) return;
  MethodTransformer trans_method(method);
  for (auto str_opcode : jumbo_mismatches) {
    str_opcode->set_opcode(
      str_opcode->jumbo() ? OPCODE_CONST_STRING : OPCODE_CONST_STRING_JUMBO);
  }
}

static void fix_jumbos(DexClasses* classes, DexOutputIdx* dodx) {
  walk_methods(*classes, [&](DexMethod* m) { fix_method_jumbos(m, dodx); });
}

void DexOutput::init_header_offsets() {
  memcpy(hdr.magic, DEX_HEADER_DEXMAGIC, sizeof(hdr.magic));
  insert_map_item(TYPE_HEADER_ITEM, 1, 0);

  m_offset = hdr.header_size = sizeof(dex_header);
  hdr.endian_tag = ENDIAN_CONSTANT;
  /* Link section was never used */
  hdr.link_size = hdr.link_off = 0;
  hdr.string_ids_size = (uint32_t) dodx->stringsize();
  hdr.string_ids_off = hdr.string_ids_size ? m_offset : 0;
  insert_map_item(TYPE_STRING_ID_ITEM, (uint32_t) dodx->stringsize(), m_offset);

  m_offset += dodx->stringsize() * sizeof(dex_string_id);
  hdr.type_ids_size = (uint32_t) dodx->typesize();
  hdr.type_ids_off = hdr.type_ids_size ? m_offset : 0;
  insert_map_item(TYPE_TYPE_ID_ITEM, (uint32_t) dodx->typesize(), m_offset);

  m_offset += dodx->typesize() * sizeof(dex_type_id);
  hdr.proto_ids_size = (uint32_t) dodx->protosize();
  hdr.proto_ids_off = hdr.proto_ids_size ? m_offset : 0;
  insert_map_item(TYPE_PROTO_ID_ITEM, (uint32_t) dodx->protosize(), m_offset);

  m_offset += dodx->protosize() * sizeof(dex_proto_id);
  hdr.field_ids_size = (uint32_t) dodx->fieldsize();
  hdr.field_ids_off = hdr.field_ids_size ? m_offset : 0;
  insert_map_item(TYPE_FIELD_ID_ITEM, (uint32_t) dodx->fieldsize(), m_offset);

  m_offset += dodx->fieldsize() * sizeof(dex_field_id);
  hdr.method_ids_size = (uint32_t) dodx->methodsize();
  hdr.method_ids_off = hdr.method_ids_size ? m_offset : 0;
  insert_map_item(TYPE_METHOD_ID_ITEM, (uint32_t) dodx->methodsize(), m_offset);

  m_offset += dodx->methodsize() * sizeof(dex_method_id);
  hdr.class_defs_size = (uint32_t) m_classes->size();
  hdr.class_defs_off = hdr.class_defs_size ? m_offset : 0;
  insert_map_item(TYPE_CLASS_DEF_ITEM, (uint32_t) m_classes->size(), m_offset);

  m_offset += m_classes->size() * sizeof(dex_class_def);
  hdr.data_off = m_offset;
  /* Todo... */
  hdr.map_off = 0;
  hdr.data_size = 0;
  hdr.file_size = 0;
}

void DexOutput::finalize_header() {
  hdr.data_size = m_offset - hdr.data_off;
  hdr.file_size = m_offset;
  int skip;
  skip = sizeof(hdr.magic) + sizeof(hdr.checksum) + sizeof(hdr.signature);
  memcpy(m_output, &hdr, sizeof(hdr));
  Sha1Context context;
  sha1_init(&context);
  sha1_update(&context, m_output + skip, hdr.file_size - skip);
  sha1_final(hdr.signature, &context);
  memcpy(m_output, &hdr, sizeof(hdr));
  uint32_t adler = (uint32_t)adler32(0L, Z_NULL, 0);
  skip = sizeof(hdr.magic) + sizeof(hdr.checksum);
  adler = (uint32_t) adler32(adler, (const Bytef*)(m_output + skip), hdr.file_size - skip);
  hdr.checksum = adler;
  memcpy(m_output, &hdr, sizeof(hdr));
}

namespace {

void write_method_mapping(
  std::string filename,
  const DexOutputIdx* dodx,
  const ProguardMap& proguard_map,
  size_t dex_number
) {
  if (filename.empty()) return;
  FILE* fd = fopen(filename.c_str(), "a");
  assert_log(fd, "Can't open method mapping file %s: %s\n",
             filename.c_str(),
             strerror(errno));
  for (auto& it : dodx->method_to_idx()) {
    auto method = it.first;
    auto idx = it.second;

    // Types (and methods) internal to our app have a cached deobfuscated name
    // that comes from the proguard map.  If we don't have one, it's a
    // system/framework class, so we can just return the name.
    auto const& typecls = method->get_class();
    auto const& cls = type_class(typecls);
    auto deobf_class = [&] {
      if (cls) {
        auto deobname = cls->get_deobfuscated_name();
        if (!deobname.empty()) return deobname;
      }
      return proguard_name(typecls);
    }();

    // Some method refs aren't "concrete" (e.g., referring to a method defined
    // by a superclass via a subclass).  We only know how to deobfuscate
    // concrete names, so resolve this ref to an actual definition.
    auto resolved_method = [&] {
      if (cls) {
        auto intf_mr = resolve_intf_methodref(method);
        if (intf_mr) return intf_mr;
        auto resm = resolve_method(method, MethodSearch::Any);
        if (resm) return resm;
      }
      return method;
    }();

    // Consult the cached method names, or just give it back verbatim.
    auto deobf_method = [&] {
      auto deobfname = resolved_method->get_deobfuscated_name();
      if (!deobfname.empty()) return deobfname;
      return proguard_name(resolved_method);
    }();

    // Format is <cls>.<name>:(<args>)<ret>
    // We only want the name here.
    auto begin = deobf_method.find('.') + 1;
    auto end = deobf_method.rfind(':');
    auto deobf_method_name = deobf_method.substr(begin, end-begin);

    fprintf(fd,
            "%u %lu %s %s\n",
            idx,
            dex_number,
            deobf_method_name.c_str(),
            deobf_class.c_str());
  }
  fclose(fd);
}

void write_class_mapping(
  std::string filename,
  DexClasses* classes,
  const size_t class_defs_size,
  const ProguardMap& proguard_map,
  const size_t dex_number
) {
  if (filename.empty()) return;
  FILE* fd = fopen(filename.c_str(), "a");

  for (uint32_t idx = 0; idx < class_defs_size; idx++) {

    DexClass* cls = classes->get(idx);
    auto deobf_class = [&] {
      if (cls) {
        auto deobname = cls->get_deobfuscated_name();
        if (!deobname.empty()) return deobname;
      }
      return proguard_name(cls);
    }();

    fprintf(fd,
            "%u %lu %s\n",
            idx,
            dex_number,
            deobf_class.c_str());
  }

  fclose(fd);
}

} // namespace

void DexOutput::write_symbol_files() {
  write_method_mapping(
    m_method_mapping_filename,
    dodx,
    m_config_files.get_proguard_map(),
    m_dex_number
  );
  write_class_mapping(
    m_class_mapping_filename,
    m_classes,
    hdr.class_defs_size,
    m_config_files.get_proguard_map(),
    m_dex_number
  );
}

void DexOutput::prepare(SortMode string_mode, SortMode code_mode) {
  fix_jumbos(m_classes, dodx);
  init_header_offsets();
  generate_static_values();
  generate_typelist_data();
  generate_string_data(string_mode);
  generate_code_items(code_mode);
  generate_class_data_items();
  generate_type_data();
  generate_proto_data();
  generate_field_data();
  generate_method_data();
  generate_class_data();
  generate_annotations();
  generate_debug_items();
  generate_map();
  align_output();
  finalize_header();
}

void DexOutput::write() {
  struct stat st;
  int fd = open(m_filename, O_CREAT | O_TRUNC | O_WRONLY, 0660);
  if (fd == -1) {
    perror("Error writing dex");
    return;
  }
  ::write(fd, m_output, m_offset);
  if (0 == fstat(fd, &st)) {
    m_stats.num_bytes = st.st_size;
  }
  close(fd);

  write_symbol_files();
}

dex_output_stats_t
write_classes_to_dex(
  std::string filename,
  DexClasses* classes,
  LocatorIndex* locator_index,
  size_t dex_number,
  ConfigFiles& cfg,
  const Json::Value& json_cfg,
  PositionMapper* pos_mapper)
{
  auto method_mapping_filename = json_cfg.get("method_mapping", "").asString();
  auto class_mapping_filename = json_cfg.get("class_mapping", "").asString();
  auto sort_strings = json_cfg.get("string_sort_mode", "").asString();
  auto sort_bytecode = json_cfg.get("bytecode_sort_mode", "").asString();
  SortMode string_sort_mode = DEFAULT;
  SortMode code_sort_mode = DEFAULT;
  if (sort_strings == "class_strings") {
    string_sort_mode = CLASS_STRINGS;
  } else if (sort_strings == "class_order") {
    string_sort_mode = CLASS_ORDER;
  }
  if (sort_bytecode == "class_order") {
    code_sort_mode = CLASS_ORDER;
  }
  DexOutput dout = DexOutput(
    filename.c_str(),
    classes,
    locator_index,
    dex_number,
    cfg,
    pos_mapper,
    method_mapping_filename,
    class_mapping_filename);
  dout.prepare(string_sort_mode, code_sort_mode);
  dout.write();
  return dout.m_stats;
}

LocatorIndex
make_locator_index(const DexClassesVector& dexen)
{
  LocatorIndex index;

  uint32_t dexnr = 1; // Zero is reserved for Android classes
  for (auto dexit = dexen.begin(); dexit != dexen.end(); ++dexit, ++dexnr) {
    const DexClasses& classes = *dexit;
    uint32_t clsnr = 0;
    for (auto clsit = classes.begin();
         clsit != classes.end();
         ++clsit, ++clsnr)
    {
      DexString* clsname = (*clsit)->get_type()->get_name();
      bool inserted = index.insert(
        std::make_pair(clsname, Locator::make(dexnr, clsnr)))
        .second;
      // We shouldn't see the same class defined in two dexen
      assert(inserted);
      (void) inserted; // Shut up compiler when defined(NDEBUG)
    }
  }

  return index;
}

dex_output_stats_t&
  operator+=(dex_output_stats_t& lhs, const dex_output_stats_t& rhs) {
  lhs.num_types += rhs.num_types;
  lhs.num_classes += rhs.num_classes;
  lhs.num_methods += rhs.num_methods;
  lhs.num_method_refs += rhs.num_method_refs;
  lhs.num_fields += rhs.num_fields;
  lhs.num_field_refs += rhs.num_field_refs;
  lhs.num_strings += rhs.num_strings;
  lhs.num_protos += rhs.num_protos;
  lhs.num_static_values += rhs.num_static_values;
  lhs.num_annotations += rhs.num_annotations;
  lhs.num_type_lists += rhs.num_type_lists;
  lhs.num_bytes += rhs.num_bytes;
  return lhs;
}
