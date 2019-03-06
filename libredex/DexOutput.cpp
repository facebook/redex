/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <algorithm>
#include <assert.h>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <list>
#include <memory>
#include <stdlib.h>
#include <sys/stat.h>
#include <unordered_set>

#ifdef _MSC_VER
// TODO: Rewrite open/write/close with C/C++ standards. But it works for now.
#include <io.h>
#define open _open
#define write _write
#define close _close
#define fstat _fstat64i32
#define stat _stat64i32
#define O_CREAT _O_CREAT
#define O_TRUNC _O_TRUNC
#define O_WRONLY _O_WRONLY
#endif

#include "Debug.h"
#include "DexClass.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "IODIMetadata.h"
#include "IRCode.h"
#include "Pass.h"
#include "Resolver.h"
#include "Sha1.h"
#include "Trace.h"
#include "Walkers.h"
#include "WorkQueue.h"

/*
 * For adler32...
 */
#include <zlib.h>

template<class T, class U>
class CustomSort {
  private:
    const std::unordered_map<const T*, unsigned int>& m_map;
    U m_cmp;

  public:
    CustomSort(const std::unordered_map<const T*, unsigned int>& input_map, U cmp)
    : m_map(input_map) {
      m_cmp = cmp;
    }

    bool operator()(const T* a, const T* b) const {
      bool a_in = m_map.count(a);
      bool b_in = m_map.count(b);
      if (!a_in && !b_in) {
        return m_cmp(a,b);
      } else if (a_in && b_in) {
        auto const a_idx = m_map.at(a);
        auto const b_idx = m_map.at(b);
        if (a_idx != b_idx) {
          return a_idx < b_idx;
        } else {
          return m_cmp(a,b);
        }
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

std::vector<DexMethod*> GatheredTypes::get_dexmethod_emitlist() {
  std::vector<DexMethod*> methlist;
  for (auto cls : *m_classes) {
    TRACE(OPUT, 3, "[dexmethod_emitlist][class] %s\n", cls->c_str());
    auto const& dmethods = cls->get_dmethods();
    auto const& vmethods = cls->get_vmethods();
    if (traceEnabled(OPUT, 3)) {
      for (const auto &dmeth : dmethods) {
        TRACE(OPUT, 3, "  [dexmethod_emitlist][dmethod] %s\n", dmeth->c_str());
      }
      for (const auto &vmeth : vmethods) {
        TRACE(OPUT, 3, "  [dexmethod_emitlist][dmethod] %s\n", vmeth->c_str());
      }
    }
    methlist.insert(methlist.end(), dmethods.begin(), dmethods.end());
    methlist.insert(methlist.end(), vmethods.begin(), vmethods.end());
  }
  return methlist;
}

void GatheredTypes::sort_dexmethod_emitlist_default_order(
    std::vector<DexMethod*>& lmeth) {
  std::stable_sort(lmeth.begin(), lmeth.end(), compare_dexmethods);
}

void GatheredTypes::sort_dexmethod_emitlist_cls_order(
    std::vector<DexMethod*>& lmeth) {
  std::stable_sort(lmeth.begin(), lmeth.end(),
                   CustomSort<DexMethod, cmp_dmethod>(m_methods_in_cls_order,
                                                      compare_dexmethods));
}

void GatheredTypes::sort_dexmethod_emitlist_profiled_order(
    std::vector<DexMethod*>& lmeth) {
  std::stable_sort(
      lmeth.begin(),
      lmeth.end(),
      dexmethods_profiled_comparator(&m_method_to_weight,
                                     &m_method_sorting_whitelisted_substrings));
}

void GatheredTypes::sort_dexmethod_emitlist_clinit_order(
    std::vector<DexMethod*>& lmeth) {
  std::stable_sort(lmeth.begin(), lmeth.end(),
                   [](const DexMethod* a, const DexMethod* b) {
                     if (is_clinit(a) && !is_clinit(b)) {
                       return true;
                     } else {
                       return false;
                     }
                   });
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
  std::vector<DexProto*> protos;
  for (auto const& m : m_lmethod) {
    protos.push_back(m->get_proto());
  }
  std::sort(protos.begin(), protos.end());
  protos.erase(std::unique(protos.begin(), protos.end()), protos.end());
  std::sort(protos.begin(), protos.end(), cmp);
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
    walk::methods(v, [&](DexMethod* m) {
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
  unsigned int index = 0;
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

CodeItemEmit::CodeItemEmit(DexMethod* meth, DexCode* c, dex_code_item* ci)
    : method(meth), code(c), code_item(ci) {}

DexOutput::DexOutput(
    const char* path,
    DexClasses* classes,
    LocatorIndex* locator_index,
    bool emit_name_based_locators,
    size_t store_number,
    size_t dex_number,
    DebugInfoKind debug_info_kind,
    IODIMetadata* iodi_metadata,
    const ConfigFiles& config_files,
    PositionMapper* pos_mapper,
    std::unordered_map<DexMethod*, uint64_t>* method_to_id,
    std::unordered_map<DexCode*, std::vector<DebugLineItem>>* code_debug_lines,
    const std::string& method_mapping_filename,
    const std::string& class_mapping_filename,
    const std::string& pg_mapping_filename,
    const std::string& bytecode_offset_filename)
    : m_config_files(config_files) {
  m_classes = classes;
  m_iodi_metadata = iodi_metadata;
  m_output = (uint8_t*)malloc(k_max_dex_size);
  memset(m_output, 0, k_max_dex_size);
  m_offset = 0;
  m_gtypes = new GatheredTypes(classes);
  dodx = m_gtypes->get_dodx(m_output);
  m_filename = path;
  m_pos_mapper = pos_mapper;
  m_method_to_id = method_to_id;
  m_code_debug_lines = code_debug_lines;
  m_method_mapping_filename = method_mapping_filename;
  m_class_mapping_filename = class_mapping_filename;
  m_pg_mapping_filename = pg_mapping_filename;
  m_bytecode_offset_filename = bytecode_offset_filename;
  m_store_number = store_number;
  m_dex_number = dex_number;
  m_locator_index = locator_index;
  m_emit_name_based_locators = emit_name_based_locators;
  m_debug_info_kind = debug_info_kind;
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
  size_t locator_length = locator.encode(buf);
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
  if (m_emit_name_based_locators) {
    const char* s = descriptor->c_str();
    uint32_t global_clsnr = Locator::decodeGlobalClassIndex(s);
    if (global_clsnr != Locator::invalid_global_class_index) {
      // We don't need locators for renamed classes since
      // name-based-locators are enabled.
      return nullptr;
    }
  }

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
      return std::unique_ptr<Locator>(new Locator(Locator::make(0, 0, 0)));
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
  if (mode == SortMode::CLASS_ORDER) {
    TRACE(CUSTOMSORT, 2, "using class order for string pool sorting\n");
    string_order = m_gtypes->get_cls_order_dexstring_emitlist();
  } else if (mode == SortMode::CLASS_STRINGS) {
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
  size_t locators = 0;
  for (DexString* str : string_order) {
    if (locator_for_descriptor(type_names, str)) {
      ++locators;
    }
  }

  if (m_emit_name_based_locators) {
    locators += 2;
    always_assert(dodx->stringidx(DexString::make_string("")) == 0);
  }

  size_t nrstr = string_order.size() + locators;

  insert_map_item(TYPE_STRING_DATA_ITEM, (uint32_t)nrstr, m_offset);
  for (DexString* str : string_order) {
    // Emit lookup acceleration string if requested
    std::unique_ptr<Locator> locator = locator_for_descriptor(type_names, str);
    if (locator) {
      unsigned orig_offset = m_offset;
      emit_locator(*locator);
      locator_size += m_offset - orig_offset;
    }

    // Emit name-based lookup acceleration information for string with index 0
    // if requested
    uint32_t idx = dodx->stringidx(str);
    if (idx == 0 && m_emit_name_based_locators) {
      always_assert(!locator);
      unsigned orig_offset = m_offset;
      emit_name_based_locators();
      locator_size += m_offset - orig_offset;
    }

    // Emit the string itself
    TRACE(CUSTOMSORT, 3, "str emit %s\n", SHOW(str));
    stringids[idx].offset = m_offset;
    str->encode(m_output + m_offset);
    m_offset += str->get_entry_size();
    m_stats.num_strings++;
  }

  if (m_locator_index != nullptr || m_emit_name_based_locators) {
    TRACE(LOC, 2, "Used %u bytes for %u locator strings\n", locator_size,
          locators);
  }
}

void DexOutput::emit_name_based_locators() {
  uint global_class_indices_first = Locator::invalid_global_class_index;
  uint global_class_indices_last = Locator::invalid_global_class_index;

  // We decode all class names --- to find the first and last renamed one,
  // and also check that all renamed names are indeed in the right place.
  dex_class_def* cdefs = (dex_class_def*)(m_output + hdr.class_defs_off);
  for (uint32_t i = 0; i < hdr.class_defs_size; i++) {
    DexClass* clz = m_classes->at(i);
    const char* str = clz->get_name()->c_str();
    uint32_t global_clsnr = Locator::decodeGlobalClassIndex(str);
    TRACE(LOC, 3, "Class %s has global class index %u\n", str, global_clsnr);
    if (global_clsnr != Locator::invalid_global_class_index) {
      global_class_indices_last = global_clsnr;
      if (global_class_indices_first == Locator::invalid_global_class_index) {
        // First time we come across a properly renamed class - let's store the
        // global_class_indices_first.
        // Note that the first class in this dex might not actually be a renamed
        // class. But we want our class loaders to be able to determine a the
        // actual class table index of a class by simply subtracting a number.
        // So we set global_class_indices_first to be the global class index of
        // the actual first class the dex, which was the class i iterations
        // earlier.
        global_class_indices_first = global_clsnr - i;
      } else {
        always_assert_log(
            global_clsnr == global_class_indices_first + i,
            "Out of order global class index: got %u, expected %u\n",
            global_clsnr, global_class_indices_first + i);
      }
    }
  }

  TRACE(LOC, 2,
        "Global class indices for store %u, dex %u: first %u, last %u\n",
        m_store_number, m_dex_number, global_class_indices_first,
        global_class_indices_last);

  if (global_class_indices_first != Locator::invalid_global_class_index) {
    // Emit two strings:

    // 1. Locator for the last renamed class in this Dex
    emit_locator(
        Locator(m_store_number, m_dex_number + 1, global_class_indices_last));

    // 2. Locator for what would be the first class in this Dex
    //    (see comment for computation of global_class_indices_first above)
    emit_locator(
        Locator(m_store_number, m_dex_number + 1, global_class_indices_first));
  } else {
    // Dummy locators
    emit_locator(Locator(0, 0, 0));
    emit_locator(Locator(0, 0, 0));
  }
}

void DexOutput::generate_type_data() {
  // Check that we don't have more than 2 ^ 15 type refs in one dex.
  //
  // NOTE: This is required because of a bug found in Android up to 7.
  constexpr const size_t kMaxTypeRefs = 1 << 15;
  always_assert_log(
      dodx->type_to_idx().size() < kMaxTypeRefs,
      "Trying to encode too many type refs in dex %lu: %lu (limit: %lu).\n"
      "NOTE: Please check InterDexPass config flags and set: "
      "`type_refs_limit: 32768`",
      m_dex_number,
      dodx->type_to_idx().size(),
      kMaxTypeRefs);

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
    DexClass* clz = m_classes->at(i);
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
    m_stats.num_field_refs++;

  }
}

void DexOutput::generate_method_data() {
  constexpr size_t kMaxMethodRefs = 64 * 1024;
  constexpr size_t kMaxFieldRefs = 64 * 1024;
  always_assert_log(
      dodx->method_to_idx().size() <= kMaxMethodRefs,
      "Trying to encode too many method refs in dex %lu: %lu (limit: %lu)",
      m_dex_number,
      dodx->method_to_idx().size(),
      kMaxMethodRefs);
  always_assert_log(
      dodx->field_to_idx().size() <= kMaxFieldRefs,
      "Trying to encode too many field refs in dex %lu: %lu (limit: %lu)",
      m_dex_number,
      dodx->field_to_idx().size(),
      kMaxFieldRefs);
  auto methodids = (dex_method_id*)(m_output + hdr.method_ids_off);
  for (auto& it : dodx->method_to_idx()) {
    auto method = it.first;
    auto idx = it.second;
    methodids[idx].classidx = dodx->typeidx(method->get_class());
    methodids[idx].protoidx = dodx->protoidx(method->get_proto());
    methodids[idx].nameidx = dodx->stringidx(method->get_name());
    m_stats.num_method_refs++;
  }
}

void DexOutput::generate_class_data() {
  dex_class_def* cdefs = (dex_class_def*)(m_output + hdr.class_defs_off);
  for (uint32_t i = 0; i < hdr.class_defs_size; i++) {
    m_stats.num_classes++;
    DexClass* clz = m_classes->at(i);
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
    m_stats.num_fields +=
        clz->get_ifields().size() + clz->get_sfields().size();
    m_stats.num_methods +=
        clz->get_vmethods().size() + clz->get_dmethods().size();
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
    uint32_t offset = (uint32_t)(((uint8_t*)it.code_item) - m_output);
    dco[it.code] = offset;
  }
  for (uint32_t i = 0; i < hdr.class_defs_size; i++) {
    DexClass* clz = m_classes->at(i);
    if (!clz->has_class_data()) continue;
    /* No alignment constraints for this data */
    int size = clz->encode(dodx, dco, m_output + m_offset);
    m_cdi_offsets[clz] = m_offset;
    m_offset += size;
  }
  insert_map_item(TYPE_CLASS_DATA_ITEM, (uint32_t) m_cdi_offsets.size(), cdi_start);
}

static void sync_all(const Scope& scope) {
  constexpr bool serial = false; // for debugging
  auto wq = workqueue_foreach<DexMethod*>([](DexMethod* m){m->sync();});
  walk::code(scope,
            [](DexMethod*) { return true; },
            [&](DexMethod* m, IRCode&) {
              if (serial) {
                TRACE(MTRANS, 2, "Syncing %s\n", SHOW(m));
                m->sync();
              } else {
                wq.add_item(m);
              }
            });
  wq.run_all();
}

void DexOutput::generate_code_items(const std::vector<SortMode>& mode) {
  TRACE(MAIN, 2, "generate_code_items\n");
  /*
   * Optimization note:  We should pass a sort routine to the
   * emitlist to optimize pagecache efficiency.
   */
  align_output();
  uint32_t ci_start = m_offset;
  sync_all(*m_classes);

  // Get all methods.
  std::vector<DexMethod*> lmeth = m_gtypes->get_dexmethod_emitlist();

  // Repeatedly perform stable sorts starting with the last (least important)
  // sorting method specified.
  for (auto it = mode.rbegin(); it != mode.rend(); ++it) {
    switch (*it) {
      case SortMode::CLASS_ORDER:
        TRACE(CUSTOMSORT, 2, "using class order for bytecode sorting\n");
        m_gtypes->sort_dexmethod_emitlist_cls_order(lmeth);
        break;
      case SortMode::METHOD_PROFILED_ORDER:
        TRACE(CUSTOMSORT, 2,
              "using method profiled order for bytecode sorting\n");
        m_gtypes->sort_dexmethod_emitlist_profiled_order(lmeth);
        break;
      case SortMode::CLINIT_FIRST:
        TRACE(CUSTOMSORT, 2,
              "sorting <clinit> sections before all other bytecode");
        m_gtypes->sort_dexmethod_emitlist_clinit_order(lmeth);
        break;

      case SortMode::CLASS_STRINGS:
        TRACE(CUSTOMSORT, 2,
              "Unsupport bytecode sorting method SortMode::CLASS_STRINGS\n");
        break;
      case SortMode::DEFAULT:
        TRACE(CUSTOMSORT, 2, "using default sorting order\n");
        m_gtypes->sort_dexmethod_emitlist_default_order(lmeth);
        break;
      }
  }
  for (DexMethod* meth : lmeth) {
    if (meth->get_access() & (ACC_ABSTRACT | ACC_NATIVE)) {
      // There is no code item for ABSTRACT or NATIVE methods.
      continue;
    }
    TRACE(CUSTOMSORT, 3, "method emit %s %s\n", SHOW(meth->get_class()), SHOW(meth));
    DexCode* code = meth->get_dex_code();
    always_assert_log(
        meth->is_concrete() && code != nullptr,
        "Undefined method in generate_code_items()\n\t prototype: %s\n", SHOW(meth));
    align_output();
    int size = code->encode(dodx, (uint32_t*)(m_output + m_offset));
    check_method_instruction_size_limit(m_config_files, size, SHOW(meth));
    m_method_bytecode_offsets.emplace_back(meth->get_name()->c_str(), m_offset);
    m_code_item_emits.emplace_back(meth, code,
                                   (dex_code_item*)(m_output + m_offset));
    m_offset += size;
    m_stats.num_instructions += code->get_instructions().size();
  }
  insert_map_item(TYPE_CODE_ITEM, (uint32_t) m_code_item_emits.size(), ci_start);
}

void DexOutput::check_method_instruction_size_limit(const ConfigFiles& cfg,
                                                    int size,
                                                    const char* method_name) {
  always_assert_log(size >= 0, "Size of method cannot be negative: %d\n", size);

  uint32_t instruction_size_bitwidth_limit =
      cfg.get_instruction_size_bitwidth_limit();

  if (instruction_size_bitwidth_limit) {
    uint64_t hard_instruction_size_limit = 1L
                                           << instruction_size_bitwidth_limit;
    always_assert_log(
        ((uint64_t)size) <= hard_instruction_size_limit,
        "Size of method exceeded limit. size: %d, limit: %d, method: %s\n",
        size, hard_instruction_size_limit, method_name);
  }
}

void DexOutput::generate_static_values() {
  uint32_t sv_start = m_offset;
  std::unordered_map<DexEncodedValueArray,
                     uint32_t,
                     boost::hash<DexEncodedValueArray>>
      enc_arrays;
  for (uint32_t i = 0; i < hdr.class_defs_size; i++) {
    DexClass* clz = m_classes->at(i);
    // Fields need to be sorted otherwise static values may end up out of order
    auto& sfields = clz->get_sfields();
    std::sort(sfields.begin(), sfields.end(), compare_dexfields);
    auto& ifields = clz->get_ifields();
    std::sort(ifields.begin(), ifields.end(), compare_dexfields);
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
                             std::vector<DexAnnotationDirectory*>& adirlist) {
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
  std::vector<DexAnnotationDirectory*> lad;
  int xrefsize = 0;
  int annodirsize = 0;
  int xrefcnt = 0;
  std::map<DexAnnotationDirectory*, int> ad_to_classnum;
  annomap_t annomap;
  asetmap_t asetmap;
  xrefmap_t xrefmap;
  adirmap_t adirmap;

  for (uint32_t i = 0; i < hdr.class_defs_size; i++) {
    DexClass* clz = m_classes->at(i);
    DexAnnotationDirectory* ad = clz->get_annotation_directory();
    if (ad) {
      xrefsize += ad->xref_size();
      annodirsize += ad->annodir_size();
      xrefcnt += ad->xref_count();
      lad.push_back(ad);
      ad_to_classnum[ad] = i;
    }
  }
  std::sort(lad.begin(), lad.end(), annotation_cmp);
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

namespace {
int emit_debug_info(
    DexOutputIdx* dodx,
    bool emit_positions,
    DexDebugItem* dbg,
    DexCode* dc,
    dex_code_item* dci,
    PositionMapper* pos_mapper,
    uint8_t* output,
    uint32_t offset,
    std::unordered_map<DexCode*, std::vector<DebugLineItem>>* dbg_lines) {
  // No align requirement for debug items.
  std::vector<DebugLineItem> debug_line_info;
  uint32_t line_start{0};
  auto dbgops = generate_debug_instructions(dbg, pos_mapper, &line_start,
                                            &debug_line_info);
  int size = 0;
  if (emit_positions) {
    size = dbg->encode(dodx, output + offset, line_start, dbgops);
    dci->debug_info_off = offset;
  }
  if (dbg_lines != nullptr) {
    (*dbg_lines)[dc] = debug_line_info;
  }
  return size;
}

// Returns a DexDebugInstruction corresponding to emitting a line entry
// with the given address offset and line offset. Asserts if invalid arguments.
inline std::unique_ptr<DexDebugInstruction> create_line_entry(int8_t line,
                                                              uint8_t addr) {
  // These are limits imposed by
  // https://source.android.com/devices/tech/dalvik/dex-format#opcodes
  always_assert(line >= -4 && line <= 10);
  always_assert(addr <= 17);
  // Below is correct because adjusted_opcode = (addr * 15) + (line + 4), so
  // line_offset = -4 + (adjusted_opcode % 15) = -4 + line + 4 = line
  // addr_offset = adjusted_opcode / 15 = addr * 15 / 15 = addr since line + 4
  // is bounded by 0 and 14 we know (line + 4) / 15 = 0
  uint8_t opcode = 0xa + (addr * 15) + (line + 4);
  return std::make_unique<DexDebugInstruction>(
      static_cast<DexDebugItemOpcode>(opcode));
}

uint32_t emit_instruction_offset_debug_info(
    DexOutputIdx* dodx,
    bool per_arity,
    PositionMapper* pos_mapper,
    std::vector<CodeItemEmit>& code_items,
    const IODIMetadata& iodi_metadata,
    uint8_t* output,
    uint32_t offset,
    int* dbgcount,
    std::unordered_map<DexCode*, std::vector<DebugLineItem>>* code_debug_map) {
  // Algo is as follows:
  // 1) Calculate max method size for each method of N params
  // 2) Emit one debug program that will emit a position for each pc up to
  //    size calculated in (1) for each method of N params
  // 3) Tie all code items back to debug program emitted in (2)
  //
  // If per_arity is false then all the "of N params" are replaced with a
  // calculated max param size.
  std::unordered_map<uint32_t, uint32_t> param_to_size;
  // (1)
  uint32_t max_param = 0;
  for (auto& it : code_items) {
    DexCode* dc = it.code;
    const auto dbg_item = dc->get_debug_item();
    if (!dbg_item) {
      continue;
    }
    if (!iodi_metadata.can_safely_use_iodi(it.method)) {
      continue;
    }
    uint32_t real_param_size = dbg_item->get_param_names().size();
    uint32_t param_size = per_arity ? real_param_size : 0;
    auto& max_size = param_to_size[param_size];
    auto size = dc->size();
    if (size > max_size) {
      max_size = size;
    }
    if (real_param_size > max_param) {
      max_param = real_param_size;
    }
  }
  // (2)
  std::unordered_map<uint32_t, uint32_t> param_to_offset;
  uint32_t initial_offset = offset;
  for (auto& pts : param_to_size) {
    auto param_size = per_arity ? pts.first : max_param;
    auto insns_size = pts.second;
    TRACE(OPUT, 2,
          "[emit_instruction_offset_debug_info][param_to_size] %u : %u\n",
          param_size, insns_size);
    param_to_offset[param_size] = offset;
    TRACE(OPUT, 2,
          "[emit_instruction_offset_debug_info][param_to_offset] %u : %u\n",
          param_size, offset);
    std::vector<DexString*> params;
    for (size_t i = 0; i < param_size; i++) {
      params.push_back(nullptr);
    }
    std::vector<std::unique_ptr<DexDebugInstruction>> dbgops;
    if (insns_size > 0) {
      // First emit an entry for pc = 0 -> line = 0
      dbgops.push_back(create_line_entry(0, 0));
      // Now emit an entry for each pc thereafter
      // (0x1e increments addr+line by 1)
      for (size_t i = 1; i < insns_size; i++) {
        dbgops.push_back(create_line_entry(1, 1));
      }
    }
    offset += DexDebugItem::encode(nullptr, output + offset, 0, params, dbgops);
    *dbgcount += 1;
  }
  // (3)
  auto offset_end = param_to_offset.end();
  for (auto& it : code_items) {
    DexCode* dc = it.code;
    const auto dbg = dc->get_debug_item();
    if (!dbg) {
      continue;
    }
    dex_code_item* dci = it.code_item;
    bool use_iodi = iodi_metadata.can_safely_use_iodi(it.method);
    // We still want to fill in pos_mapper and code_debug_map, so run the
    // usual code to emit debug info, additionally we actual emit the usual
    // debug info if we can't safely use iodi.
    offset += emit_debug_info(dodx, !use_iodi, dbg, dc, dci, pos_mapper, output,
                              offset, code_debug_map);
    if (use_iodi) {
      uint32_t param_size =
          per_arity ? dbg->get_param_names().size() : max_param;
      auto offset_it = param_to_offset.find(param_size);
      always_assert_log(offset_it != offset_end,
                        "Expected to find param to offset");
      dci->debug_info_off = offset_it->second;
    } else {
      *dbgcount += 1;
    }
  }
  // Return how much data we've encoded
  return offset - initial_offset;
}

} // namespace

void DexOutput::generate_debug_items() {
  uint32_t dbg_start = m_offset;
  int dbgcount = 0;
  bool per_arity =
      m_debug_info_kind == DebugInfoKind::InstructionOffsetsPerArity;
  bool emit_positions = m_debug_info_kind != DebugInfoKind::NoPositions;
  bool use_iodi =
      m_debug_info_kind == DebugInfoKind::InstructionOffsets || per_arity;
  if (use_iodi && m_iodi_metadata) {
    m_offset += emit_instruction_offset_debug_info(dodx,
                                                   per_arity,
                                                   m_pos_mapper,
                                                   m_code_item_emits,
                                                   *m_iodi_metadata,
                                                   m_output,
                                                   m_offset,
                                                   &dbgcount,
                                                   m_code_debug_lines);
  } else {
    if (use_iodi) {
      fprintf(stderr,
              "[IODI] WARNING: Not using IODI because no iodi metadata file was"
              " specified.\n");
    }
    for (auto& it : m_code_item_emits) {
      DexCode* dc = it.code;
      dex_code_item* dci = it.code_item;
      auto dbg = dc->get_debug_item();
      if (dbg == nullptr) continue;
      dbgcount++;
      m_offset +=
          emit_debug_info(dodx, emit_positions, dbg, dc, dci, m_pos_mapper,
                          m_output, m_offset, m_code_debug_lines);
    }
  }
  if (emit_positions) {
    insert_map_item(TYPE_DEBUG_INFO_ITEM, dbgcount, dbg_start);
  }
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
  auto code = method->get_code();
  if (!code) return; // nothing to do for native methods

  for (auto& mie : *code) {
    if (mie.type != MFLOW_DEX_OPCODE) {
      continue;
    }
    auto insn = mie.dex_insn;
    auto op = insn->opcode();
    if (op != DOPCODE_CONST_STRING && op != DOPCODE_CONST_STRING_JUMBO) {
      continue;
    }

    auto str = static_cast<DexOpcodeString*>(insn)->get_string();
    uint32_t stridx = dodx->stringidx(str);
    bool jumbo = ((stridx >> 16) != 0);

    if (jumbo) {
      insn->set_opcode(DOPCODE_CONST_STRING_JUMBO);
    } else if (!jumbo) {
      insn->set_opcode(DOPCODE_CONST_STRING);
    }
  }
}

static void fix_jumbos(DexClasses* classes, DexOutputIdx* dodx) {
  walk::methods(*classes, [&](DexMethod* m) { fix_method_jumbos(m, dodx); });
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
  const std::string& filename,
  const DexOutputIdx* dodx,
  const DexClasses* classes,
  uint8_t* dex_signature,
  std::unordered_map<DexMethod*, uint64_t>* method_to_id
) {
  if (filename.empty()) return;
  FILE* fd = fopen(filename.c_str(), "a");
  assert_log(fd, "Can't open method mapping file %s: %s\n",
             filename.c_str(),
             strerror(errno));
  std::unordered_set<DexClass*> classes_in_dex(classes->begin(), classes->end());
  for (auto& it : dodx->method_to_idx()) {
    auto method = it.first;
    auto idx = it.second;

    // Types (and methods) internal to our app have a cached deobfuscated name
    // that comes from the proguard map.  If we don't have one, it's a
    // system/framework class, so we can just return the name.
    auto const& typecls = method->get_class();
    auto const& cls = type_class(typecls);
    if (classes_in_dex.count(cls) == 0) {
      // We only want to emit IDs for the methods that are defined in this dex,
      // and not for references to methods in other dexes.
      continue;
    }
    auto deobf_class = [&] {
      if (cls) {
        auto deobname = cls->get_deobfuscated_name();
        if (!deobname.empty()) return deobname;
      }
      return show(typecls);
    }();

    // Some method refs aren't "concrete" (e.g., referring to a method defined
    // by a superclass via a subclass).  We only know how to deobfuscate
    // concrete names, so resolve this ref to an actual definition.
    auto resolved_method = [&]() -> DexMethodRef* {
      if (cls) {
        auto resm = resolve_method(method,
            is_interface(cls) ? MethodSearch::Interface : MethodSearch::Any);
        if (resm) return resm;
      }
      return method;
    }();

    // Consult the cached method names, or just give it back verbatim.
    auto deobf_method = [&] {
      if (resolved_method->is_def()) {
        auto deobfname =
            static_cast<DexMethod*>(resolved_method)->get_deobfuscated_name();
        if (!deobfname.empty()) return deobfname;
      }
      return show(resolved_method);
    }();

    // Format is <cls>.<name>:(<args>)<ret>
    // We only want the name here.
    auto begin = deobf_method.find('.') + 1;
    auto end = deobf_method.rfind(':');
    auto deobf_method_name = deobf_method.substr(begin, end-begin);

    //
    // Turns out, the checksum can change on-device. (damn you dexopt)
    // The signature, however, is never recomputed. Let's log the top 4 bytes,
    // in little-endian (since that's faster to compute on-device).
    //
    uint32_t signature = *reinterpret_cast<uint32_t*>(dex_signature);

    if (method_to_id != nullptr) {
      if (resolved_method == method) {
        // Not recording it if method reference is not referring to
        // concrete method, otherwise will have key overlapped.
        auto dexmethod = static_cast<DexMethod*>(resolved_method);
        (*method_to_id)[dexmethod] =
            ((uint64_t)idx << 32) | (uint64_t)signature;
      }
    }

    fprintf(fd, "%u %u %s %s\n",
            idx,
            signature,
            deobf_method_name.c_str(),
            deobf_class.c_str());
  }
  fclose(fd);
}

void write_class_mapping(
  const std::string& filename,
  DexClasses* classes,
  const size_t class_defs_size,
  uint8_t* dex_signature
) {
  if (filename.empty()) return;
  FILE* fd = fopen(filename.c_str(), "a");

  for (uint32_t idx = 0; idx < class_defs_size; idx++) {

    DexClass* cls = classes->at(idx);
    auto deobf_class = [&] {
      if (cls) {
        auto deobname = cls->get_deobfuscated_name();
        if (!deobname.empty()) return deobname;
      }
      return show(cls);
    }();

    //
    // See write_method_mapping above for why checksum is insufficient.
    //
    uint32_t signature = *reinterpret_cast<uint32_t*>(dex_signature);
    fprintf(fd, "%u %u %s\n", idx, signature, deobf_class.c_str());
  }

  fclose(fd);
}

const char* deobf_primitive(char type) {
  switch (type) {
    case 'B':
      return "byte";
    case 'C':
      return "char";
    case 'D':
      return "double";
    case 'F':
      return "float";
    case 'I':
      return "int";
    case 'J':
      return "long";
    case 'S':
      return "short";
    case 'Z':
      return "boolean";
    case 'V':
      return "void";
    default:
      always_assert_log(false, "Illegal type: %c", type);
      not_reached();
  }
}

void write_pg_mapping(const std::string& filename, DexClasses* classes) {
  if (filename.empty()) return;

  auto deobf_class = [&](DexClass* cls) {
    if (cls) {
      auto deobname = cls->get_deobfuscated_name();
      if (!deobname.empty()) return deobname;
    }
    return show(cls);
  };

  auto deobf_type = [&](DexType* type) {
    if (type) {
      if (is_array(type)) {
        auto* type_str = type->c_str();
        int dim = 0;
        while (type_str[dim] == '[') {
          dim++;
        }
        DexType* inner_type = DexType::get_type(&type_str[dim]);
        DexClass* inner_cls = inner_type ? type_class(inner_type) : nullptr;
        std::string result;
        if (inner_cls) {
          result = JavaNameUtil::internal_to_external(deobf_class(inner_cls));
        } else if (inner_type && is_primitive(inner_type)) {
          result = deobf_primitive(type_str[dim]);
        } else {
          result = JavaNameUtil::internal_to_external(&type_str[dim]);
        }
        for (int i = 0 ; i < dim ; ++i) {
          result = result + "[]";
        }
        return result;
      } else {
        DexClass* cls = type_class(type);
        if (cls) {
          return JavaNameUtil::internal_to_external(deobf_class(cls));
        } else if (is_primitive(type)) {
          return std::string(deobf_primitive(type->c_str()[0]));
        } else {
          return JavaNameUtil::internal_to_external(type->c_str());
        }
      }
    }
    return show(type);
  };

  auto deobf_meth = [&](DexMethod* method) {
    if (method) {
      // Example: 672:672:boolean customShouldDelayInitMessage(android.os.Handler,android.os.Message)
      auto* proto = method->get_proto();
      std::ostringstream ss;
      auto* code = method->get_dex_code();
      auto* dbg = code ? code->get_debug_item() : nullptr;
      if (dbg) {
        uint32_t line_start = code->get_debug_item()->get_line_start();
        uint32_t line_end = line_start;
        for (auto& entry : dbg->get_entries()) {
          if (entry.type == DexDebugEntryType::Position) {
            if (entry.pos->line > line_end) {
              line_end = entry.pos->line;
            }
          }
        }
        // Treat anything bigger than 2^31 as 0
        if (line_start > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
          line_start = 0;
        }
        if (line_end > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
          line_end = 0;
        }
        ss << line_start << ":" << line_end << ":";
      }
      auto* rtype = proto->get_rtype();
      auto rtype_str = deobf_type(rtype);
      ss << rtype_str;
      ss << " " << method->get_simple_deobfuscated_name() << "(";
      auto args = proto->get_args()->get_type_list();
      for (auto iter = args.begin() ; iter != args.end() ; ++iter) {
        auto* atype = *iter;
        auto atype_str = deobf_type(atype);
        ss << atype_str;
        if (iter + 1 != args.end()) {
          ss << ",";
        }
      }
      ss << ")";
      return ss.str();
    }
    return show(method);
  };

  auto deobf_field = [&](DexField* field) {
    if (field) {
      std::ostringstream ss;
      ss << deobf_type(field->get_type())
      << " "
      << field->get_simple_deobfuscated_name();
      return ss.str();
    }
    return show(field);
  };

  std::ofstream ofs(filename.c_str(), std::ofstream::out | std::ofstream::app);

  for (auto cls : *classes) {
    auto deobf_cls = deobf_class(cls);
    ofs << JavaNameUtil::internal_to_external(deobf_cls) << " -> "
        << JavaNameUtil::internal_to_external(cls->get_type()->c_str())
        << ":" << std::endl;
    for (auto field : cls->get_ifields()) {
      auto deobf = deobf_field(field);
      ofs << "    " << deobf << " -> " << field->c_str() << std::endl;
    }
    for (auto field : cls->get_sfields()) {
      auto deobf = deobf_field(field);
      ofs << "    " << deobf << " -> " << field->c_str() << std::endl;
    }
    for (auto meth : cls->get_dmethods()) {
      auto deobf = deobf_meth(meth);
      ofs << "    " << deobf << " -> " << meth->c_str() << std::endl;
    }
    for (auto meth : cls->get_vmethods()) {
      auto deobf = deobf_meth(meth);
      ofs << "    " << deobf << " -> " << meth->c_str() << std::endl;
    }
  }
}

void write_bytecode_offset_mapping(
  const std::string& filename,
  const std::vector<std::pair<std::string, uint32_t>>& method_offsets
) {
  if (filename.empty()) { return; }

  auto fd = fopen(filename.c_str(), "a");
  assert_log(fd, "Can't open bytecode offset file %s: %s\n",
             filename.c_str(),
             strerror(errno));

  for (const auto& item : method_offsets) {
    fprintf(fd, "%u %s\n", item.second, item.first.c_str());
  }

  fclose(fd);
}

} // namespace

void DexOutput::write_symbol_files() {
  write_method_mapping(
    m_method_mapping_filename,
    dodx,
    m_classes,
    hdr.signature,
    m_method_to_id
  );
  write_class_mapping(
    m_class_mapping_filename,
    m_classes,
    hdr.class_defs_size,
    hdr.signature
  );
  write_pg_mapping(
    m_pg_mapping_filename,
    m_classes
  );
  write_bytecode_offset_mapping(m_bytecode_offset_filename,
                                m_method_bytecode_offsets);
}

void GatheredTypes::set_method_sorting_whitelisted_substrings(
    const std::unordered_set<std::string>& whitelisted_substrings) {
  m_method_sorting_whitelisted_substrings = whitelisted_substrings;
}

void GatheredTypes::set_method_to_weight(
    const std::unordered_map<std::string, unsigned int>& method_to_weight) {
  m_method_to_weight = method_to_weight;
}

void DexOutput::prepare(SortMode string_mode,
                        const std::vector<SortMode>& code_mode,
                        const ConfigFiles& cfg) {

  if (std::find(code_mode.begin(), code_mode.end(),
                SortMode::METHOD_PROFILED_ORDER) != code_mode.end()) {
    m_gtypes->set_method_to_weight(cfg.get_method_to_weight());
    m_gtypes->set_method_sorting_whitelisted_substrings(
        cfg.get_method_sorting_whitelisted_substrings());
  }

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

static SortMode make_sort_bytecode(const std::string& sort_bytecode) {
  if (sort_bytecode == "class_order") {
    return SortMode::CLASS_ORDER;
  } else if (sort_bytecode == "clinit_order") {
    return SortMode::CLINIT_FIRST;
  } else if (sort_bytecode == "method_profiled_order") {
    return SortMode::METHOD_PROFILED_ORDER;
  } else {
    return SortMode::DEFAULT;
  }
}

static DebugInfoKind deserialize_debug_info_kind(std::string raw_kind) {
  if (raw_kind == "no_positions") {
    return DebugInfoKind::NoPositions;
  } else if (raw_kind == "iodi") {
    return DebugInfoKind::InstructionOffsets;
  } else if (raw_kind == "iodi_per_arity") {
    return DebugInfoKind::InstructionOffsetsPerArity;
  } else {
    always_assert_log(raw_kind == "normal" || raw_kind == "",
                      "Unknown debug info kind. Supported kinds are \"normal\","
                      " \"no_positions\", \"iodi\", \"iodi_per_arity\".");
    return DebugInfoKind::Normal;
  }
}

dex_stats_t write_classes_to_dex(
    std::string filename,
    DexClasses* classes,
    LocatorIndex* locator_index,
    bool emit_name_based_locators,
    size_t store_number,
    size_t dex_number,
    const ConfigFiles& cfg,
    PositionMapper* pos_mapper,
    std::unordered_map<DexMethod*, uint64_t>* method_to_id,
    std::unordered_map<DexCode*, std::vector<DebugLineItem>>* code_debug_lines,
    IODIMetadata* iodi_metadata) {
  const JsonWrapper& json_cfg = cfg.get_json_config();
  auto method_mapping_filename =
      cfg.metafile(json_cfg.get("method_mapping", std::string()));
  auto class_mapping_filename =
      cfg.metafile(json_cfg.get("class_mapping", std::string()));
  auto pg_mapping_filename =
      cfg.metafile(json_cfg.get("proguard_map_output", std::string()));
  auto bytecode_offset_filename =
      cfg.metafile(json_cfg.get("bytecode_offset_map", std::string()));
  auto sort_strings = json_cfg.get("string_sort_mode", std::string());
  DebugInfoKind debug_info_kind = deserialize_debug_info_kind(
      json_cfg.get("debug_info_kind", std::string()));
  SortMode string_sort_mode = SortMode::DEFAULT;
  if (sort_strings == "class_strings") {
    string_sort_mode = SortMode::CLASS_STRINGS;
  } else if (sort_strings == "class_order") {
    string_sort_mode = SortMode::CLASS_ORDER;
  }

  auto sort_bytecode_cfg = json_cfg.get("bytecode_sort_mode", Json::Value());
  std::vector<SortMode> code_sort_mode;

  if (sort_bytecode_cfg.isString()) {
    code_sort_mode.push_back(make_sort_bytecode(sort_bytecode_cfg.asString()));
  } else if (sort_bytecode_cfg.isArray()) {
    for (auto val : sort_bytecode_cfg) {
      code_sort_mode.push_back(make_sort_bytecode(val.asString()));
    }
  }
  if (code_sort_mode.empty()) {
    code_sort_mode.push_back(SortMode::DEFAULT);
  }

  TRACE(OPUT, 2, "[write_classes_to_dex][filename] %s\n", filename.c_str());

  DexOutput dout = DexOutput(filename.c_str(),
                             classes,
                             locator_index,
                             emit_name_based_locators,
                             store_number,
                             dex_number,
                             debug_info_kind,
                             iodi_metadata,
                             cfg,
                             pos_mapper,
                             method_to_id,
                             code_debug_lines,
                             method_mapping_filename,
                             class_mapping_filename,
                             pg_mapping_filename,
                             bytecode_offset_filename);

  dout.prepare(string_sort_mode, code_sort_mode, cfg);
  dout.write();
  return dout.m_stats;
}

LocatorIndex make_locator_index(DexStoresVector& stores,
                                bool emit_name_based_locators) {
  LocatorIndex index;

  for (uint32_t strnr = 0; strnr < stores.size(); strnr++) {
    DexClassesVector& dexen = stores[strnr].get_dexen();
    uint32_t dexnr = 1; // Zero is reserved for Android classes
    for (auto dexit = dexen.begin(); dexit != dexen.end(); ++dexit, ++dexnr) {
      const DexClasses& classes = *dexit;
      uint32_t clsnr = 0;
      for (auto clsit = classes.begin(); clsit != classes.end();
           ++clsit, ++clsnr) {
        DexString* clsname = (*clsit)->get_type()->get_name();
        if (emit_name_based_locators) {
          const auto cstr = clsname->c_str();
          uint32_t global_clsnr = Locator::decodeGlobalClassIndex(cstr);
          if (global_clsnr != Locator::invalid_global_class_index) {
            TRACE(LOC, 3,
                  "%s (%u, %u, %u) needs no locator; global class index=%u\n",
                  cstr, strnr, dexnr, clsnr, global_clsnr);
            // This prefix is followed by the global class index; this case
            // doesn't need a locator since emit_name_based_locators is enabled.
            continue;
          }
        }

        bool inserted = index
                            .insert(std::make_pair(
                                clsname, Locator::make(strnr, dexnr, clsnr)))
                            .second;
        // We shouldn't see the same class defined in two dexen
        always_assert_log(inserted, "This was already inserted %s\n",
                          (*clsit)->get_deobfuscated_name().c_str());
        (void) inserted; // Shut up compiler when defined(NDEBUG)
      }
    }
  }

  return index;
}
