/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexOutput.h"

#include <algorithm>
#include <assert.h>
#include <boost/filesystem.hpp>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <inttypes.h>
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
#include "DexCallSite.h"
#include "DexClass.h"
#include "DexInstruction.h"
#include "DexLimits.h"
#include "DexMethodHandle.h"
#include "DexPosition.h"
#include "DexUtil.h"
#include "GlobalConfig.h"
#include "IODIMetadata.h"
#include "IRCode.h"
#include "Macros.h"
#include "MethodProfiles.h"
#include "MethodSimilarityOrderer.h"
#include "Pass.h"
#include "Resolver.h"
#include "Sha1.h"
#include "Show.h"
#include "StlUtil.h"
#include "Trace.h"
#include "Walkers.h"
#include "WorkQueue.h"

/*
 * For adler32...
 */
#include <zlib.h>

template <class T, class U>
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
      return m_cmp(a, b);
    } else if (a_in && b_in) {
      auto const a_idx = m_map.at(a);
      auto const b_idx = m_map.at(b);
      if (a_idx != b_idx) {
        return a_idx < b_idx;
      } else {
        return m_cmp(a, b);
      }
    } else if (a_in) {
      return true;
    } else {
      return false;
    }
  }
};

GatheredTypes::GatheredTypes(DexClasses* classes) : m_classes(classes) {
  // ensure that the string id table contains the empty string, which is used
  // for the DexPosition mapping
  m_lstring.push_back(DexString::make_string(""));

  // build maps for the different custom sorting options
  build_cls_load_map();
  build_cls_map();
  build_method_map();

  gather_components(m_lstring, m_ltype, m_lfield, m_lmethod, m_lcallsite,
                    m_lmethodhandle, *m_classes);
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
      m_cls_load_strings, compare_dexstrings));
}

std::vector<DexString*> GatheredTypes::keep_cls_strings_together_emitlist() {
  return get_dexstring_emitlist(
      CustomSort<DexString, cmp_dstring>(m_cls_strings, compare_dexstrings));
}

std::vector<DexMethodHandle*> GatheredTypes::get_dexmethodhandle_emitlist() {
  return m_lmethodhandle;
}

std::vector<DexCallSite*> GatheredTypes::get_dexcallsite_emitlist() {
  return m_lcallsite;
}

std::vector<DexMethod*> GatheredTypes::get_dexmethod_emitlist() {
  std::vector<DexMethod*> methlist;
  for (auto cls : *m_classes) {
    TRACE(OPUT, 3, "[dexmethod_emitlist][class] %s", cls->c_str());
    auto const& dmethods = cls->get_dmethods();
    auto const& vmethods = cls->get_vmethods();
    if (traceEnabled(OPUT, 3)) {
      for (const auto& dmeth : dmethods) {
        TRACE(OPUT, 3, "  [dexmethod_emitlist][dmethod] %s", dmeth->c_str());
      }
      for (const auto& vmeth : vmethods) {
        TRACE(OPUT, 3, "  [dexmethod_emitlist][dmethod] %s", vmeth->c_str());
      }
    }
    methlist.insert(methlist.end(), dmethods.begin(), dmethods.end());
    methlist.insert(methlist.end(), vmethods.begin(), vmethods.end());
  }
  return methlist;
}

void GatheredTypes::sort_dexmethod_emitlist_method_similarity_order(
    std::vector<DexMethod*>& lmeth) {
  // We keep "perf sensitive methods" together in front, and only order by
  // similarity the remaining methods. Here, we consider as "perf sensitive
  // methods" any methods in a class that...
  // - is perf sensitive, which in particular includes all classes that are
  //   ordered by beta maps
  // - contains methods that contain any profiled methods with a very
  //   conservative min-appear cut-off.
  //
  // This is similar to the exclusions that the InterDex pass applies when
  // sorting remaining classes for better compression.
  std::unordered_set<DexType*> perf_sensitive_classes;

  std::unique_ptr<method_profiles::dexmethods_profiled_comparator> comparator{
      nullptr};

  // Some builds might not have method profiles information.
  if (m_config != nullptr) {
    MethodProfileOrderingConfig* config =
        m_config->get_global_config()
            .get_config_by_name<MethodProfileOrderingConfig>(
                "method_profile_order");

    auto& method_profiles = m_config->get_method_profiles();

    if (config != nullptr && method_profiles.is_initialized()) {
      comparator =
          std::make_unique<method_profiles::dexmethods_profiled_comparator>(
              lmeth,
              /*method_profiles=*/&method_profiles,
              config);
    }
  }

  for (auto meth : lmeth) {
    auto cls = type_class(meth->get_class());
    if (cls->is_perf_sensitive()) {
      perf_sensitive_classes.insert(meth->get_class());
      continue;
    }

    if (comparator == nullptr) {
      continue;
    }
    auto method_sort_num = comparator->get_overall_method_sort_num(meth);
    if (method_sort_num <
        method_profiles::dexmethods_profiled_comparator::VERY_END) {
      perf_sensitive_classes.insert(meth->get_class());
    }
  }

  std::vector<DexMethod*> perf_sensitive_methods;
  std::vector<DexMethod*> remaining_methods;
  for (auto meth : lmeth) {
    if (perf_sensitive_classes.count(meth->get_class())) {
      perf_sensitive_methods.push_back(meth);
    } else {
      remaining_methods.push_back(meth);
    }
  }

  TRACE(
      OPUT, 2,
      "Skipping %zu perf sensitive methods, ordering %zu methods by similarity",
      perf_sensitive_methods.size(), remaining_methods.size());
  MethodSimilarityOrderer method_similarity_orderer;
  method_similarity_orderer.order(remaining_methods);

  lmeth.clear();
  lmeth.insert(lmeth.end(), perf_sensitive_methods.begin(),
               perf_sensitive_methods.end());
  lmeth.insert(lmeth.end(), remaining_methods.begin(), remaining_methods.end());
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
  // Use std::ref to avoid comparator copies.
  redex_assert(m_config != nullptr);

  MethodProfileOrderingConfig* config =
      m_config->get_global_config()
          .get_config_by_name<MethodProfileOrderingConfig>(
              "method_profile_order");

  method_profiles::dexmethods_profiled_comparator comparator(
      lmeth,
      /*method_profiles=*/&m_config->get_method_profiles(),
      config);
  std::stable_sort(lmeth.begin(), lmeth.end(), std::ref(comparator));
}

void GatheredTypes::sort_dexmethod_emitlist_clinit_order(
    std::vector<DexMethod*>& lmeth) {
  std::stable_sort(lmeth.begin(), lmeth.end(),
                   [](const DexMethod* a, const DexMethod* b) {
                     if (method::is_clinit(a) && !method::is_clinit(b)) {
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
  std::vector<DexTypeList*>* typelist = get_typelist_list(proto);
  dexcallsite_to_idx* callsite = get_callsite_index();
  dexmethodhandle_to_idx* methodhandle = get_methodhandle_index();
  return new DexOutputIdx(string, type, proto, field, method, typelist,
                          callsite, methodhandle, base);
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
  for (auto const& c : m_lcallsite) {
    protos.push_back(c->method_type());
    for (auto arg : c->args()) {
      // n.b. how deep could this recursion go? what if there was a method
      // handle here?
      if (arg->evtype() == DEVT_METHOD_TYPE) {
        protos.push_back(((DexEncodedValueMethodType*)arg)->proto());
      }
    }
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

std::vector<DexTypeList*>* GatheredTypes::get_typelist_list(
    dexproto_to_idx* protos, cmp_dtypelist cmp) {
  std::vector<DexTypeList*>* typel = new std::vector<DexTypeList*>();
  auto class_defs_size = (uint32_t)m_classes->size();
  typel->reserve(protos->size() + class_defs_size +
                 m_additional_ltypelists.size());

  for (auto& it : *protos) {
    auto proto = it.first;
    typel->push_back(proto->get_args());
  }
  for (uint32_t i = 0; i < class_defs_size; i++) {
    DexClass* clz = m_classes->at(i);
    typel->push_back(clz->get_interfaces());
  }
  typel->insert(typel->end(),
                m_additional_ltypelists.begin(),
                m_additional_ltypelists.end());
  sort_unique(*typel, compare_dextypelists);
  return typel;
}

dexcallsite_to_idx* GatheredTypes::get_callsite_index(cmp_callsite cmp) {
  std::sort(m_lcallsite.begin(), m_lcallsite.end(), cmp);
  dexcallsite_to_idx* csidx = new dexcallsite_to_idx();
  uint32_t idx = 0;
  for (auto it = m_lcallsite.begin(); it != m_lcallsite.end(); it++) {
    csidx->insert(std::make_pair(*it, idx++));
  }
  return csidx;
}

dexmethodhandle_to_idx* GatheredTypes::get_methodhandle_index(
    cmp_methodhandle cmp) {
  std::sort(m_lmethodhandle.begin(), m_lmethodhandle.end(), cmp);
  dexmethodhandle_to_idx* mhidx = new dexmethodhandle_to_idx();
  uint32_t idx = 0;
  for (auto it = m_lmethodhandle.begin(); it != m_lmethodhandle.end(); it++) {
    mhidx->insert(std::make_pair(*it, idx++));
  }
  return mhidx;
}

void GatheredTypes::build_cls_load_map() {
  unsigned int index = 0;
  int type_strings = 0;
  int init_strings = 0;
  int total_strings = 0;
  for (const auto& cls : *m_classes) {
    // gather type first, assuming class load will check all components of a
    // class first
    std::vector<DexType*> cls_types;
    cls->gather_types(cls_types);
    std::sort(cls_types.begin(), cls_types.end(), compare_dextypes);
    for (const auto& t : cls_types) {
      if (!m_cls_load_strings.count(t->get_name())) {
        m_cls_load_strings[t->get_name()] = index;
        index++;
        type_strings++;
      }
    }
    // now add in any strings found in <clinit>
    // since they are likely to be accessed during class load
    for (const auto& m : cls->get_dmethods()) {
      if (method::is_clinit(m)) {
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
    // locality if a random class in a dex is loaded and then executes some
    // methods
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

  TRACE(CUSTOMSORT, 1,
        "found %d strings from types, %d from strings in init methods, %d "
        "total strings",
        type_strings, init_strings, total_strings);
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

namespace {

// Leave 250K empty as a margin to not overrun.
constexpr uint32_t k_output_red_zone = 250000;

constexpr uint32_t k_default_max_dex_size = 32 * 1024 * 1024;

uint32_t get_dex_output_size(const ConfigFiles& conf) {
  size_t output_size;
  conf.get_json_config().get("dex_output_buffer_size", k_default_max_dex_size,
                             output_size);
  return (uint32_t)output_size;
}

} // namespace

CodeItemEmit::CodeItemEmit(DexMethod* meth, DexCode* c, dex_code_item* ci)
    : method(meth), code(c), code_item(ci) {}

namespace {
// DO NOT CHANGE THESE VALUES! Many services will break if you do.
constexpr const char* METHOD_MAPPING = "redex-method-id-map.txt";
constexpr const char* CLASS_MAPPING = "redex-class-id-map.txt";
constexpr const char* BYTECODE_OFFSET_MAPPING = "redex-bytecode-offset-map.txt";
constexpr const char* REDEX_PG_MAPPING = "redex-class-rename-map.txt";
constexpr const char* REDEX_FULL_MAPPING = "redex-full-rename-map.txt";
} // namespace

DexOutput::DexOutput(
    const char* path,
    DexClasses* classes,
    std::shared_ptr<GatheredTypes> gtypes,
    LocatorIndex* locator_index,
    bool normal_primary_dex,
    size_t store_number,
    size_t dex_number,
    DebugInfoKind debug_info_kind,
    IODIMetadata* iodi_metadata,
    const ConfigFiles& config_files,
    PositionMapper* pos_mapper,
    std::unordered_map<DexMethod*, uint64_t>* method_to_id,
    std::unordered_map<DexCode*, std::vector<DebugLineItem>>* code_debug_lines,
    PostLowering* post_lowering,
    int min_sdk)
    : m_classes(classes),
      m_gtypes(std::move(gtypes)),
      // Required because the BytecodeDebugger setting creates huge amounts
      // of debug information (multiple dex debug entries per instruction)
      m_output_size((debug_info_kind == DebugInfoKind::BytecodeDebugger
                         ? get_dex_output_size(config_files) * 2
                         : get_dex_output_size(config_files)) +
                    k_output_red_zone),
      m_output(std::make_unique<uint8_t[]>(m_output_size)),
      m_offset(0),
      m_iodi_metadata(iodi_metadata),
      m_config_files(config_files),
      m_min_sdk(min_sdk) {
  // Ensure a clean slate.
  memset(m_output.get(), 0, m_output_size);

  m_dodx = std::make_unique<DexOutputIdx>(*m_gtypes->get_dodx(m_output.get()));

  always_assert_log(
      m_dodx->method_to_idx().size() <= kMaxMethodRefs,
      "Trying to encode too many method refs in dex %s: %lu (limit: %lu). Run "
      "with `-J ir_type_checker.check_num_of_refs=true`.",
      boost::filesystem::path(path).filename().c_str(),
      m_dodx->method_to_idx().size(),
      kMaxMethodRefs);
  always_assert_log(
      m_dodx->field_to_idx().size() <= kMaxFieldRefs,
      "Trying to encode too many field refs in dex %s: %lu (limit: %lu). Run "
      "with `-J ir_type_checker.check_num_of_refs=true`.",
      boost::filesystem::path(path).filename().c_str(),
      m_dodx->field_to_idx().size(),
      kMaxFieldRefs);

  m_filename = path;
  m_pos_mapper = pos_mapper;
  m_method_to_id = method_to_id;
  m_code_debug_lines = code_debug_lines;
  m_method_mapping_filename = config_files.metafile(METHOD_MAPPING);
  m_class_mapping_filename = config_files.metafile(CLASS_MAPPING);
  m_pg_mapping_filename = config_files.metafile(REDEX_PG_MAPPING);
  m_full_mapping_filename = config_files.metafile(REDEX_FULL_MAPPING);
  m_bytecode_offset_filename = config_files.metafile(BYTECODE_OFFSET_MAPPING);
  m_store_number = store_number;
  m_dex_number = dex_number;
  m_locator_index = locator_index;
  m_normal_primary_dex = normal_primary_dex;
  m_debug_info_kind = debug_info_kind;
  if (post_lowering) {
    m_detached_methods = post_lowering->get_detached_methods();
  }
}

void DexOutput::insert_map_item(uint16_t maptype,
                                uint32_t size,
                                uint32_t offset,
                                uint32_t bytes) {
  if (size == 0) return;
  dex_map_item item{};
  item.type = maptype;
  item.size = size;
  item.offset = offset;
  m_map_items.emplace_back(item);

  switch (maptype) {
  case TYPE_HEADER_ITEM:
    m_stats.header_item_count += size;
    m_stats.header_item_bytes += bytes;
    break;
  case TYPE_STRING_ID_ITEM:
    m_stats.string_id_count += size;
    m_stats.string_id_bytes += bytes;
    break;
  case TYPE_TYPE_ID_ITEM:
    m_stats.type_id_count += size;
    m_stats.type_id_bytes += bytes;
    break;
  case TYPE_PROTO_ID_ITEM:
    m_stats.proto_id_count += size;
    m_stats.proto_id_bytes += bytes;
    break;
  case TYPE_FIELD_ID_ITEM:
    m_stats.field_id_count += size;
    m_stats.field_id_bytes += bytes;
    break;
  case TYPE_METHOD_ID_ITEM:
    m_stats.method_id_count += size;
    m_stats.method_id_bytes += bytes;
    break;
  case TYPE_CLASS_DEF_ITEM:
    m_stats.class_def_count += size;
    m_stats.class_def_bytes += bytes;
    break;
  case TYPE_CALL_SITE_ID_ITEM:
    m_stats.call_site_id_count += size;
    m_stats.call_site_id_bytes += bytes;
    break;
  case TYPE_METHOD_HANDLE_ITEM:
    m_stats.method_handle_count += size;
    m_stats.method_handle_bytes += bytes;
    break;
  case TYPE_MAP_LIST:
    m_stats.map_list_count += size;
    m_stats.map_list_bytes += bytes;
    break;
  case TYPE_TYPE_LIST:
    m_stats.type_list_count += size;
    m_stats.type_list_bytes += bytes;
    break;
  case TYPE_ANNOTATION_SET_REF_LIST:
    m_stats.annotation_set_ref_list_count += size;
    m_stats.annotation_set_ref_list_bytes += bytes;
    break;
  case TYPE_ANNOTATION_SET_ITEM:
    m_stats.annotation_set_count += size;
    m_stats.annotation_set_bytes += bytes;
    break;
  case TYPE_CLASS_DATA_ITEM:
    m_stats.class_data_count += size;
    m_stats.class_data_bytes += bytes;
    break;
  case TYPE_CODE_ITEM:
    m_stats.code_count += size;
    m_stats.code_bytes += bytes;
    break;
  case TYPE_STRING_DATA_ITEM:
    m_stats.string_data_count += size;
    m_stats.string_data_bytes += bytes;
    break;
  case TYPE_DEBUG_INFO_ITEM:
    m_stats.debug_info_count += size;
    m_stats.debug_info_bytes += bytes;
    break;
  case TYPE_ANNOTATION_ITEM:
    m_stats.annotation_count += size;
    m_stats.annotation_bytes += bytes;
    break;
  case TYPE_ENCODED_ARRAY_ITEM:
    m_stats.encoded_array_count += size;
    m_stats.encoded_array_bytes += bytes;
    break;
  case TYPE_ANNOTATIONS_DIR_ITEM:
    m_stats.annotations_directory_count += size;
    m_stats.annotations_directory_bytes += bytes;
    break;
  }
}

void DexOutput::emit_locator(Locator locator) {
  char buf[Locator::encoded_max];
  size_t locator_length = locator.encode(buf);
  write_uleb128(m_output.get() + m_offset, (uint32_t)locator_length);
  inc_offset(uleb128_encoding_size((uint32_t)locator_length));
  memcpy(m_output.get() + m_offset, buf, locator_length + 1);
  inc_offset(locator_length + 1);
}

std::unique_ptr<Locator> DexOutput::locator_for_descriptor(
    const std::unordered_set<DexString*>& type_names, DexString* descriptor) {
  LocatorIndex* locator_index = m_locator_index;
  if (locator_index != nullptr) {
    const char* s = descriptor->c_str();
    uint32_t global_clsnr = Locator::decodeGlobalClassIndex(s);
    if (global_clsnr != Locator::invalid_global_class_index) {
      // We don't need locators for renamed classes since
      // name-based-locators are enabled.
      return nullptr;
    }

    auto locator_it = locator_index->find(descriptor);
    if (locator_it != locator_index->end()) {
      // This string is the name of a type we define in one of our
      // dex files.
      return std::unique_ptr<Locator>(new Locator(locator_it->second));
    }

    if (type_names.count(descriptor)) {
      // If we're emitting an array name, see whether the element
      // type is one of ours; if so, emit a locator for that type.
      if (s[0] == '[') {
        while (*s == '[') {
          ++s;
        }
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
    TRACE(CUSTOMSORT, 2, "using class order for string pool sorting");
    string_order = m_gtypes->get_cls_order_dexstring_emitlist();
  } else if (mode == SortMode::CLASS_STRINGS) {
    TRACE(CUSTOMSORT, 2, "using class names pack for string pool sorting");
    string_order = m_gtypes->keep_cls_strings_together_emitlist();
  } else {
    TRACE(CUSTOMSORT, 2, "using default string pool sorting");
    string_order = m_gtypes->get_dexstring_emitlist();
  }
  dex_string_id* stringids =
      (dex_string_id*)(m_output.get() + hdr.string_ids_off);

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

  if (m_locator_index != nullptr) {
    locators += 3;
    always_assert(m_dodx->stringidx(DexString::make_string("")) == 0);
  }

  size_t nrstr = string_order.size() + locators;
  const uint32_t str_data_start = m_offset;

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
    uint32_t idx = m_dodx->stringidx(str);
    if (idx == 0 && m_locator_index != nullptr) {
      always_assert(!locator);
      unsigned orig_offset = m_offset;
      emit_magic_locators();
      locator_size += m_offset - orig_offset;
    }

    // Emit the string itself
    TRACE(CUSTOMSORT, 3, "str emit %s", SHOW(str));
    stringids[idx].offset = m_offset;
    str->encode(m_output.get() + m_offset);
    inc_offset(str->get_entry_size());
    m_stats.num_strings++;
  }

  insert_map_item(TYPE_STRING_DATA_ITEM, (uint32_t)nrstr, str_data_start,
                  m_offset - str_data_start);

  if (m_locator_index != nullptr) {
    TRACE(LOC, 2, "Used %u bytes for %zu locator strings", locator_size,
          locators);
  }
}

void DexOutput::emit_magic_locators() {
  uint32_t global_class_indices_first = Locator::invalid_global_class_index;
  uint32_t global_class_indices_last = Locator::invalid_global_class_index;

  // We decode all class names --- to find the first and last renamed one,
  // and also check that all renamed names are indeed in the right place.
  for (uint32_t i = 0; i < hdr.class_defs_size; i++) {
    DexClass* clz = m_classes->at(i);
    const char* str = clz->get_name()->c_str();
    uint32_t global_clsnr = Locator::decodeGlobalClassIndex(str);
    TRACE(LOC, 3, "Class %s has global class index %u", str, global_clsnr);
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
        "Global class indices for store %zu, dex %zu: first %u, last %u",
        m_store_number, m_dex_number, global_class_indices_first,
        global_class_indices_last);

  // Emit three locator strings

  if (global_class_indices_first == Locator::invalid_global_class_index) {
    // This dex defines no renamed classes. We encode this with a special
    // otherwise illegal convention:
    global_class_indices_first = 1;
    global_class_indices_last = 0;
  }

  // 1. Locator for the last renamed class in this Dex
  emit_locator(
      Locator(m_store_number, m_dex_number + 1, global_class_indices_last));

  // 2. Locator for what would be the first class in this Dex
  //    (see comment for computation of global_class_indices_first above)
  emit_locator(
      Locator(m_store_number, m_dex_number + 1, global_class_indices_first));

  // magic locator
  emit_locator(Locator(Locator::magic_strnr, Locator::magic_dexnr,
                       Locator::magic_clsnr));
}

void DexOutput::generate_type_data() {
  always_assert_log(
      m_dodx->type_to_idx().size() < get_max_type_refs(m_min_sdk),
      "Trying to encode too many type refs in dex %lu: %lu (limit: %lu).\n"
      "NOTE: Please check InterDexPass config flags and set: "
      "`reserved_trefs: %lu` (or larger, until the issue goes away)",
      m_dex_number,
      m_dodx->type_to_idx().size(),
      get_max_type_refs(m_min_sdk),
      m_dodx->type_to_idx().size() - get_max_type_refs(m_min_sdk));

  dex_type_id* typeids = (dex_type_id*)(m_output.get() + hdr.type_ids_off);
  for (auto& p : m_dodx->type_to_idx()) {
    auto t = p.first;
    auto idx = p.second;
    typeids[idx].string_idx = m_dodx->stringidx(t->get_name());
    m_stats.num_types++;
  }
}

void DexOutput::generate_typelist_data() {
  std::vector<DexTypeList*>& typel = m_dodx->typelist_list();
  uint32_t tl_start = align(m_offset);
  size_t num_tls = 0;
  for (DexTypeList* tl : typel) {
    if (tl->get_type_list().empty()) {
      m_tl_emit_offsets[tl] = 0;
      continue;
    }
    ++num_tls;
    align_output();
    m_tl_emit_offsets[tl] = m_offset;
    int size = tl->encode(m_dodx.get(), (uint32_t*)(m_output.get() + m_offset));
    inc_offset(size);
    m_stats.num_type_lists++;
  }
  /// insert_map_item returns early if num_tls is zero
  insert_map_item(TYPE_TYPE_LIST, (uint32_t)num_tls, tl_start,
                  m_offset - tl_start);
}

void DexOutput::generate_proto_data() {
  auto protoids = (dex_proto_id*)(m_output.get() + hdr.proto_ids_off);
  for (auto& it : m_dodx->proto_to_idx()) {
    auto proto = it.first;
    auto idx = it.second;
    protoids[idx].shortyidx = m_dodx->stringidx(proto->get_shorty());
    protoids[idx].rtypeidx = m_dodx->typeidx(proto->get_rtype());
    protoids[idx].param_off = m_tl_emit_offsets.at(proto->get_args());
    m_stats.num_protos++;
  }
}

void DexOutput::generate_field_data() {
  auto fieldids = (dex_field_id*)(m_output.get() + hdr.field_ids_off);
  for (auto& it : m_dodx->field_to_idx()) {
    auto field = it.first;
    auto idx = it.second;
    fieldids[idx].classidx = m_dodx->typeidx(field->get_class());
    fieldids[idx].typeidx = m_dodx->typeidx(field->get_type());
    fieldids[idx].nameidx = m_dodx->stringidx(field->get_name());
    m_stats.num_field_refs++;
  }
}

void DexOutput::generate_method_data() {
  auto methodids = (dex_method_id*)(m_output.get() + hdr.method_ids_off);
  for (auto& it : m_dodx->method_to_idx()) {
    auto method = it.first;
    auto idx = it.second;
    methodids[idx].classidx = m_dodx->typeidx(method->get_class());
    methodids[idx].protoidx = m_dodx->protoidx(method->get_proto());
    methodids[idx].nameidx = m_dodx->stringidx(method->get_name());
    m_stats.num_method_refs++;
  }
}

void DexOutput::generate_class_data() {
  dex_class_def* cdefs = (dex_class_def*)(m_output.get() + hdr.class_defs_off);
  for (uint32_t i = 0; i < hdr.class_defs_size; i++) {
    m_stats.num_classes++;
    DexClass* clz = m_classes->at(i);
    cdefs[i].typeidx = m_dodx->typeidx(clz->get_type());
    cdefs[i].access_flags = clz->get_access();
    cdefs[i].super_idx = m_dodx->typeidx(clz->get_super_class());
    cdefs[i].interfaces_off = 0;
    cdefs[i].annotations_off = 0;
    cdefs[i].interfaces_off = m_tl_emit_offsets[clz->get_interfaces()];
    auto source_file = m_pos_mapper->get_source_file(clz);
    if (source_file != nullptr) {
      cdefs[i].source_file_idx = m_dodx->stringidx(source_file);
    } else {
      cdefs[i].source_file_idx = DEX_NO_INDEX;
    }
    if (m_static_values.count(clz)) {
      cdefs[i].static_values_off = m_static_values[clz];
    } else {
      cdefs[i].static_values_off = 0;
    }
    m_stats.num_fields += clz->get_ifields().size() + clz->get_sfields().size();
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
    uint32_t offset = (uint32_t)(((uint8_t*)it.code_item) - m_output.get());
    dco[it.code] = offset;
  }
  dex_class_def* cdefs = (dex_class_def*)(m_output.get() + hdr.class_defs_off);
  uint32_t count = 0;
  for (uint32_t i = 0; i < hdr.class_defs_size; i++) {
    DexClass* clz = m_classes->at(i);
    if (!clz->has_class_data()) continue;
    /* No alignment constraints for this data */
    int size = clz->encode(m_dodx.get(), dco, m_output.get() + m_offset);
    cdefs[i].class_data_offset = m_offset;
    inc_offset(size);
    count += 1;
  }
  insert_map_item(TYPE_CLASS_DATA_ITEM, count, cdi_start, m_offset - cdi_start);
}

static void sync_all(const Scope& scope) {
  constexpr bool serial = false; // for debugging
  auto fn = [&](DexMethod* m, IRCode&) {
    if (serial) {
      TRACE(MTRANS, 2, "Syncing %s", SHOW(m));
    }
    m->sync();
  };

  if (serial) {
    walk::code(scope, fn);
  } else {
    walk::parallel::code(scope, fn);
  }
}

void DexOutput::generate_code_items(const std::vector<SortMode>& mode) {
  TRACE(MAIN, 2, "generate_code_items");
  /*
   * Optimization note:  We should pass a sort routine to the
   * emitlist to optimize pagecache efficiency.
   */
  uint32_t ci_start = align(m_offset);
  sync_all(*m_classes);

  // Get all methods.
  std::vector<DexMethod*> lmeth = m_gtypes->get_dexmethod_emitlist();

  // Repeatedly perform stable sorts starting with the last (least important)
  // sorting method specified.
  for (auto it = mode.rbegin(); it != mode.rend(); ++it) {
    switch (*it) {
    case SortMode::CLASS_ORDER:
      TRACE(CUSTOMSORT, 2, "using class order for bytecode sorting");
      m_gtypes->sort_dexmethod_emitlist_cls_order(lmeth);
      break;
    case SortMode::METHOD_PROFILED_ORDER:
      TRACE(CUSTOMSORT, 2, "using method profiled order for bytecode sorting");
      m_gtypes->sort_dexmethod_emitlist_profiled_order(lmeth);
      break;
    case SortMode::CLINIT_FIRST:
      TRACE(CUSTOMSORT, 2,
            "sorting <clinit> sections before all other bytecode");
      m_gtypes->sort_dexmethod_emitlist_clinit_order(lmeth);
      break;
    case SortMode::CLASS_STRINGS:
      TRACE(CUSTOMSORT, 2,
            "Unsupport bytecode sorting method SortMode::CLASS_STRINGS");
      break;
    case SortMode::METHOD_SIMILARITY:
      TRACE(CUSTOMSORT, 2, "using method similarity order");
      m_gtypes->sort_dexmethod_emitlist_method_similarity_order(lmeth);
      break;
    case SortMode::DEFAULT:
      TRACE(CUSTOMSORT, 2, "using default sorting order");
      m_gtypes->sort_dexmethod_emitlist_default_order(lmeth);
      break;
    }
  }
  for (DexMethod* meth : lmeth) {
    if (meth->get_access() & (ACC_ABSTRACT | ACC_NATIVE)) {
      // There is no code item for ABSTRACT or NATIVE methods.
      continue;
    }
    TRACE(CUSTOMSORT, 3, "method emit %s %s", SHOW(meth->get_class()),
          SHOW(meth));
    DexCode* code = meth->get_dex_code();
    always_assert_log(
        meth->is_concrete() && code != nullptr,
        "Undefined method in generate_code_items()\n\t prototype: %s\n",
        SHOW(meth));
    align_output();
    int size =
        code->encode(m_dodx.get(), (uint32_t*)(m_output.get() + m_offset));
    check_method_instruction_size_limit(m_config_files, size, SHOW(meth));
    m_method_bytecode_offsets.emplace_back(meth->get_name()->c_str(), m_offset);
    m_code_item_emits.emplace_back(meth, code,
                                   (dex_code_item*)(m_output.get() + m_offset));
    auto insns_size =
        ((const dex_code_item*)(m_output.get() + m_offset))->insns_size;
    inc_offset(size);
    m_stats.num_instructions += code->get_instructions().size();
    m_stats.instruction_bytes += insns_size * 2;
  }
  /// insert_map_item returns early if m_code_item_emits is empty
  insert_map_item(TYPE_CODE_ITEM, (uint32_t)m_code_item_emits.size(), ci_start,
                  m_offset - ci_start);
}

void DexOutput::generate_callsite_data() {
  uint32_t offset =
      hdr.class_defs_off + hdr.class_defs_size * sizeof(dex_class_def);

  auto callsites = m_gtypes->get_dexcallsite_emitlist();
  dex_callsite_id* dexcallsites = (dex_callsite_id*)(m_output.get() + offset);
  for (uint32_t i = 0; i < callsites.size(); i++) {
    m_stats.num_callsites++;
    DexCallSite* callsite = callsites.at(i);
    dexcallsites[i].callsite_off = m_call_site_items[callsite];
  }
}

void DexOutput::generate_methodhandle_data() {
  uint32_t total_callsite_size =
      m_dodx->callsitesize() * sizeof(dex_callsite_id);
  uint32_t offset = hdr.class_defs_off +
                    hdr.class_defs_size * sizeof(dex_class_def) +
                    total_callsite_size;
  dex_methodhandle_id* dexmethodhandles =
      (dex_methodhandle_id*)(m_output.get() + offset);
  for (auto it : m_dodx->methodhandle_to_idx()) {
    m_stats.num_methodhandles++;
    DexMethodHandle* methodhandle = it.first;
    uint32_t idx = it.second;
    dexmethodhandles[idx].method_handle_type = methodhandle->type();
    if (DexMethodHandle::isInvokeType(methodhandle->type())) {
      dexmethodhandles[idx].field_or_method_id =
          m_dodx->methodidx(methodhandle->methodref());
    } else {
      dexmethodhandles[idx].field_or_method_id =
          m_dodx->fieldidx(methodhandle->fieldref());
    }
    dexmethodhandles[idx].unused1 = 0;
    dexmethodhandles[idx].unused2 = 0;
  }
}

void DexOutput::check_method_instruction_size_limit(const ConfigFiles& conf,
                                                    int size,
                                                    const char* method_name) {
  always_assert_log(size >= 0, "Size of method cannot be negative: %d\n", size);

  uint32_t instruction_size_bitwidth_limit =
      conf.get_instruction_size_bitwidth_limit();

  if (instruction_size_bitwidth_limit) {
    uint64_t hard_instruction_size_limit = 1L
                                           << instruction_size_bitwidth_limit;
    always_assert_log(((uint64_t)size) <= hard_instruction_size_limit,
                      "Size of method exceeded limit. size: %d, limit: %" PRIu64
                      ", method: %s\n",
                      size, hard_instruction_size_limit, method_name);
  }
}

void DexOutput::generate_static_values() {
  uint32_t sv_start = m_offset;
  std::unordered_map<DexEncodedValueArray, uint32_t,
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
      uint8_t* output = m_output.get() + m_offset;
      uint8_t* outputsv = output;
      /* No alignment requirements */
      deva->encode(m_dodx.get(), output);
      enc_arrays.emplace(std::move(*deva.release()), m_offset);
      m_static_values[clz] = m_offset;
      inc_offset(output - outputsv);
      m_stats.num_static_values++;
    }
  }
  {
    auto callsites = m_gtypes->get_dexcallsite_emitlist();
    for (uint32_t i = 0; i < callsites.size(); i++) {
      auto callsite = callsites[i];
      auto eva = callsite->as_encoded_value_array();
      uint32_t offset;
      if (enc_arrays.count(eva)) {
        offset = m_call_site_items[callsite] = enc_arrays.at(eva);
      } else {
        uint8_t* output = m_output.get() + m_offset;
        uint8_t* outputsv = output;
        eva.encode(m_dodx.get(), output);
        enc_arrays.emplace(std::move(eva), m_offset);
        offset = m_call_site_items[callsite] = m_offset;
        inc_offset(output - outputsv);
        m_stats.num_static_values++;
      }
    }
  }
  if (!m_static_values.empty() || !m_call_site_items.empty()) {
    insert_map_item(TYPE_ENCODED_ARRAY_ITEM, (uint32_t)enc_arrays.size(),
                    sv_start, m_offset - sv_start);
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
    anno->vencode(m_dodx.get(), annotation_bytes);
    if (annotation_byte_offsets.count(annotation_bytes)) {
      annomap[anno] = annotation_byte_offsets[annotation_bytes];
      continue;
    }
    /* Insert new annotation in tracking structs */
    annotation_byte_offsets[annotation_bytes] = m_offset;
    annomap[anno] = m_offset;
    /* Not a dupe, encode... */
    uint8_t* annoout = (uint8_t*)(m_output.get() + m_offset);
    memcpy(annoout, &annotation_bytes[0], annotation_bytes.size());
    inc_offset(annotation_bytes.size());
    annocnt++;
  }
  if (annocnt) {
    insert_map_item(TYPE_ANNOTATION_ITEM, annocnt, mentry_offset,
                    m_offset - mentry_offset);
  }
  m_stats.num_annotations += annocnt;
}

void DexOutput::unique_asets(annomap_t& annomap,
                             asetmap_t& asetmap,
                             std::vector<DexAnnotationSet*>& asetlist) {
  int asetcnt = 0;
  uint32_t mentry_offset = align(m_offset);
  std::map<std::vector<uint32_t>, uint32_t> aset_offsets;
  for (auto aset : asetlist) {
    if (asetmap.count(aset)) continue;
    std::vector<uint32_t> aset_bytes;
    aset->vencode(m_dodx.get(), aset_bytes, annomap);
    if (aset_offsets.count(aset_bytes)) {
      asetmap[aset] = aset_offsets[aset_bytes];
      continue;
    }
    /* Insert new aset in tracking structs */
    align_output();
    aset_offsets[aset_bytes] = m_offset;
    asetmap[aset] = m_offset;
    /* Not a dupe, encode... */
    uint8_t* asetout = (uint8_t*)(m_output.get() + m_offset);
    memcpy(asetout, &aset_bytes[0], aset_bytes.size() * sizeof(uint32_t));
    inc_offset(aset_bytes.size() * sizeof(uint32_t));
    asetcnt++;
  }
  if (asetcnt) {
    insert_map_item(TYPE_ANNOTATION_SET_ITEM, asetcnt, mentry_offset,
                    m_offset - mentry_offset);
  }
}

void DexOutput::unique_xrefs(asetmap_t& asetmap,
                             xrefmap_t& xrefmap,
                             std::vector<ParamAnnotations*>& xreflist) {
  int xrefcnt = 0;
  uint32_t mentry_offset = align(m_offset);
  std::map<std::vector<uint32_t>, uint32_t> xref_offsets;
  for (auto xref : xreflist) {
    if (xrefmap.count(xref)) continue;
    std::vector<uint32_t> xref_bytes;
    xref_bytes.push_back((unsigned int)xref->size());
    for (auto param : *xref) {
      DexAnnotationSet* das = param.second;
      always_assert_log(asetmap.count(das) != 0, "Uninitialized aset %p '%s'",
                        das, SHOW(das));
      xref_bytes.push_back(asetmap[das]);
    }
    if (xref_offsets.count(xref_bytes)) {
      xrefmap[xref] = xref_offsets[xref_bytes];
      continue;
    }
    /* Insert new xref in tracking structs */
    align_output();
    xref_offsets[xref_bytes] = m_offset;
    xrefmap[xref] = m_offset;
    /* Not a dupe, encode... */
    uint8_t* xrefout = (uint8_t*)(m_output.get() + m_offset);
    memcpy(xrefout, &xref_bytes[0], xref_bytes.size() * sizeof(uint32_t));
    inc_offset(xref_bytes.size() * sizeof(uint32_t));
    xrefcnt++;
  }
  if (xrefcnt) {
    insert_map_item(TYPE_ANNOTATION_SET_REF_LIST, xrefcnt, mentry_offset,
                    m_offset - mentry_offset);
  }
}

void DexOutput::unique_adirs(asetmap_t& asetmap,
                             xrefmap_t& xrefmap,
                             adirmap_t& adirmap,
                             std::vector<DexAnnotationDirectory*>& adirlist) {
  int adircnt = 0;
  uint32_t mentry_offset = align(m_offset);
  std::map<std::vector<uint32_t>, uint32_t> adir_offsets;
  for (auto adir : adirlist) {
    if (adirmap.count(adir)) continue;
    std::vector<uint32_t> adir_bytes;
    adir->vencode(m_dodx.get(), adir_bytes, xrefmap, asetmap);
    if (adir_offsets.count(adir_bytes)) {
      adirmap[adir] = adir_offsets[adir_bytes];
      continue;
    }
    /* Insert new adir in tracking structs */
    align_output();
    adir_offsets[adir_bytes] = m_offset;
    adirmap[adir] = m_offset;
    /* Not a dupe, encode... */
    uint8_t* adirout = (uint8_t*)(m_output.get() + m_offset);
    memcpy(adirout, &adir_bytes[0], adir_bytes.size() * sizeof(uint32_t));
    inc_offset(adir_bytes.size() * sizeof(uint32_t));
    adircnt++;
  }
  if (adircnt) {
    insert_map_item(TYPE_ANNOTATIONS_DIR_ITEM, adircnt, mentry_offset,
                    m_offset - mentry_offset);
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
  unique_asets(annomap, asetmap, asetlist);
  unique_xrefs(asetmap, xrefmap, xreflist);
  unique_adirs(asetmap, xrefmap, adirmap, lad);
  for (auto ad : lad) {
    int class_num = ad_to_classnum[ad];
    dex_class_def* cdefs =
        (dex_class_def*)(m_output.get() + hdr.class_defs_off);
    cdefs[class_num].annotations_off = adirmap[ad];
    delete ad;
  }
}

namespace {
struct DebugMetadata {
  DexDebugItem* dbg{nullptr};
  dex_code_item* dci{nullptr};
  uint32_t line_start{0};
  uint32_t num_params{0};
  uint32_t size{0};
  uint32_t dex_size{0};
  std::vector<std::unique_ptr<DexDebugInstruction>> dbgops;
};

DebugMetadata calculate_debug_metadata(
    DexDebugItem* dbg,
    DexCode* dc,
    dex_code_item* dci,
    PositionMapper* pos_mapper,
    uint32_t num_params,
    std::unordered_map<DexCode*, std::vector<DebugLineItem>>* dbg_lines,
    uint32_t line_addin) {
  std::vector<DebugLineItem> debug_line_info;
  DebugMetadata metadata;
  metadata.dbg = dbg;
  metadata.dci = dci;
  metadata.num_params = num_params;
  metadata.dbgops = generate_debug_instructions(
      dbg, pos_mapper, &metadata.line_start, &debug_line_info, line_addin);
  if (dbg_lines != nullptr) {
    (*dbg_lines)[dc] = debug_line_info;
  }
  return metadata;
}

int emit_debug_info_for_metadata(DexOutputIdx* dodx,
                                 const DebugMetadata& metadata,
                                 uint8_t* output,
                                 uint32_t offset,
                                 bool set_dci_offset = true) {
  int size = DexDebugItem::encode(dodx, output + offset, metadata.line_start,
                                  metadata.num_params, metadata.dbgops);
  if (set_dci_offset) {
    metadata.dci->debug_info_off = offset;
  }
  return size;
}

int emit_debug_info(
    DexOutputIdx* dodx,
    bool emit_positions,
    DexDebugItem* dbg,
    DexCode* dc,
    dex_code_item* dci,
    PositionMapper* pos_mapper,
    uint8_t* output,
    uint32_t offset,
    uint32_t num_params,
    std::unordered_map<DexCode*, std::vector<DebugLineItem>>* dbg_lines) {
  // No align requirement for debug items.
  DebugMetadata metadata = calculate_debug_metadata(
      dbg, dc, dci, pos_mapper, num_params, dbg_lines, /*line_addin=*/0);
  return emit_positions
             ? emit_debug_info_for_metadata(dodx, metadata, output, offset)
             : 0;
}

struct MethodKey {
  const DexMethod* method;
  uint32_t size;
};
struct MethodKeyCompare {
  // We want to sort using size as a major key and method as a minor key. The
  // minor key only exists to ensure different methods get different entries,
  // even if they have the same size as another method.
  bool operator()(const MethodKey& left, const MethodKey& right) const {
    if (left.size == right.size) {
      return compare_dexmethods(left.method, right.method);
    } else {
      return left.size > right.size;
    }
  }
};
using DebugSize = uint32_t;
using DebugMethodMap = std::map<MethodKey, DebugSize, MethodKeyCompare>;

// Iterator-like struct that gives an order of param-sizes to visit induced
// by unvisited cluster methods.
struct ParamSizeOrder {
  // This is OK. Java methods are limited to 256 parameters.
  std::bitset<257> param_size_done;

  const std::unordered_map<const DexMethod*, DebugMetadata>& method_data;

  std::vector<const DexMethod*>::const_iterator method_cur;
  std::vector<const DexMethod*>::const_iterator method_end;
  std::unordered_set<const DexMethod*> skip_methods;

  std::map<uint32_t, DebugMethodMap>::const_iterator map_cur, map_end;

  ParamSizeOrder(
      const std::unordered_map<const DexMethod*, DebugMetadata>& method_data,
      const std::vector<const DexMethod*>& methods,
      std::map<uint32_t, DebugMethodMap>::const_iterator begin,
      std::map<uint32_t, DebugMethodMap>::const_iterator end)
      : method_data(method_data),
        method_cur(methods.begin()),
        method_end(methods.end()),
        map_cur(begin),
        map_end(end) {}

  void skip(const DexMethod* m) { skip_methods.insert(m); }

  int32_t next() {
    auto get_size = [&](const DexMethod* m) {
      return method_data.at(m).num_params;
    };
    while (method_cur != method_end) {
      auto* m = *method_cur;
      ++method_cur;

      if (skip_methods.count(m) != 0) {
        continue;
      }

      auto size = get_size(m);
      if (param_size_done.test(size)) {
        continue;
      }

      param_size_done.set(size);
      return size;
    }

    while (map_cur != map_end) {
      auto size = map_cur->first;
      ++map_cur;

      if (param_size_done.test(size)) {
        continue;
      }

      param_size_done.set(size);
      return size;
    }

    return -1;
  }
};

uint32_t emit_instruction_offset_debug_info(
    DexOutputIdx* dodx,
    PositionMapper* pos_mapper,
    std::vector<CodeItemEmit*>& code_items,
    IODIMetadata& iodi_metadata,
    size_t iodi_layer,
    uint32_t line_addin,
    size_t store_number,
    size_t dex_number,
    uint8_t* output,
    uint32_t offset,
    int* dbgcount,
    std::unordered_map<DexCode*, std::vector<DebugLineItem>>* code_debug_map) {
  // Algo is as follows:
  // 1) Collect method sizes for each method of N params
  // 2) For each arity:
  //   2.1) Determine the biggest methods that we will support (see below)
  //   2.2) Emit one debug program that will emit a position for each pc up to
  //        the size calculated in 2.1
  // 3) Tie all code items back to debug program emitted in (2) and emit
  //    any normal debug info for any methods that can't use IODI (either due
  //    to being too big or being unsupported)

  // 1)
  std::map<uint32_t, DebugMethodMap> param_to_sizes;
  std::unordered_map<const DexMethod*, DebugMetadata> method_to_debug_meta;
  // We need this to calculate the size of normal debug programs for each
  // method. Hopefully no debug program is > 128k. Its ok to increase this
  // in the future.
  constexpr int TMP_SIZE = 128 * 1024;
  uint8_t* tmp = (uint8_t*)malloc(TMP_SIZE);
  std::unordered_map<const DexMethod*, std::vector<const DexMethod*>>
      clustered_methods;
  // Returns whether this is in a cluster, period, not a "current" cluster in
  // this iteration.
  auto is_in_global_cluster = [&](const DexMethod* method) {
    return iodi_metadata.get_cluster(method).size() > 1;
  };
  for (auto& it : code_items) {
    DexCode* dc = it->code;
    const auto dbg_item = dc->get_debug_item();
    redex_assert(dbg_item);
    DexMethod* method = it->method;
    redex_assert(!iodi_metadata.is_huge(method));
    uint32_t param_size = method->get_proto()->get_args()->size();
    // We still want to fill in pos_mapper and code_debug_map, so run the
    // usual code to emit debug info. We cache this and use it later if
    // it turns out we want to emit normal debug info for a given method.
    DebugMetadata metadata =
        calculate_debug_metadata(dbg_item, dc, it->code_item, pos_mapper,
                                 param_size, code_debug_map, line_addin);

    int debug_size =
        emit_debug_info_for_metadata(dodx, metadata, tmp, 0, false);
    always_assert_log(debug_size < TMP_SIZE, "Tmp buffer overrun");
    metadata.size = debug_size;
    const auto dex_size = dc->size();
    metadata.dex_size = dex_size;
    method_to_debug_meta.emplace(method, std::move(metadata));
    if (iodi_metadata.is_huge(method)) {
      continue;
    }
    auto res = param_to_sizes[param_size].emplace(MethodKey{method, dex_size},
                                                  debug_size);
    always_assert_log(res.second, "Failed to insert %s, %d pair", SHOW(method),
                      dc->size());
    if (is_in_global_cluster(method)) {
      clustered_methods[iodi_metadata.get_canonical_method(method)].push_back(
          method);
    }
  }
  free((void*)tmp);

  std20::erase_if(clustered_methods,
                  [](auto& p) { return p.second.size() <= 1; });

  std::vector<const DexMethod*> cluster_induced_order;
  for (const auto& p : clustered_methods) {
    cluster_induced_order.insert(cluster_induced_order.end(), p.second.begin(),
                                 p.second.end());
  }
  std::sort(
      cluster_induced_order.begin(), cluster_induced_order.end(),
      [&method_to_debug_meta](const DexMethod* lhs, const DexMethod* rhs) {
        if (lhs == rhs) {
          return false;
        }
        const auto& lhs_dbg = method_to_debug_meta.at(lhs);
        const auto& rhs_dbg = method_to_debug_meta.at(rhs);

        // Larger debug programs first.
        if (lhs_dbg.size != rhs_dbg.size) {
          return lhs_dbg.size > rhs_dbg.size;
        }

        // More parameters next.
        if (lhs_dbg.num_params != rhs_dbg.num_params) {
          return lhs_dbg.num_params > rhs_dbg.num_params;
        }

        // Some stable order.
        return compare_dexmethods(lhs, rhs);
      });

  ParamSizeOrder pso{method_to_debug_meta, cluster_induced_order,
                     param_to_sizes.begin(), param_to_sizes.end()};

  // 2)
  bool requires_iodi_programs =
      iodi_layer > 0 ||
      (iodi_metadata.layer_mode == IODIMetadata::IODILayerMode::kFull) ||
      (iodi_metadata.layer_mode ==
           IODIMetadata::IODILayerMode::kSkipLayer0AtApi26 &&
       iodi_metadata.min_sdk < 26) ||
      (iodi_metadata.layer_mode ==
           IODIMetadata::IODILayerMode::kAlwaysSkipLayer0ExceptPrimary &&
       store_number == 0 && dex_number == 0);
  std::unordered_map<uint32_t, std::map<uint32_t, uint32_t>> param_size_to_oset;
  uint32_t initial_offset = offset;
  for (int32_t size = pso.next(); size != -1; size = pso.next()) {
    auto param_size = size;
    const auto& dbg_sizes = param_to_sizes.at(size);

    if (dbg_sizes.empty()) {
      // May happen through cluster removal.
      continue;
    }

    // Find clustered methods in this param size.
    std::unordered_map<const DexMethod*, std::vector<MethodKey>>
        clusters_in_sizes;
    for (const auto& p : dbg_sizes) {
      clusters_in_sizes[iodi_metadata.get_canonical_method(p.first.method)]
          .push_back(p.first);
    }
    std20::erase_if(clusters_in_sizes,
                    [](auto& p) { return p.second.size() == 1; });
    size_t combinations = 1;
    for (const auto& p : clusters_in_sizes) {
      combinations *= p.second.size();
    }
    TRACE(IODI, 4, "Cluster combinations=%zu size=%zu", combinations,
          clusters_in_sizes.size());

    // 2.1) We determine the methods to use IODI we go through two filtering
    // phases:
    //   2.1.1) Filter out methods that will cause an OOM in dexlayout on
    //          Android 8+
    //   2.1.2) Filter out methods who increase uncompressed APK size

    // 2.1.1) In Android 8+ there's a background optimizer service that
    // automatically runs dex2oat with a profile collected by the runtime JIT.
    // This background optimizer includes a system called dexlayout that will
    // relocate data in order to improve locality. When relocating data it will
    // inflate debug information into an IR. This inflation currently doesn't
    // properly unique debug information that has already been inflated, and
    // instead reinflates debug information everytime a method references it.
    // Internally this vector is
    // ${number of position entries in D} * ${number of methods referencing D
    // entries long for a given debug program D. Without this filtering we've
    // found that dex2oat will OOM on most devices, resulting in no background
    // optimization (which regressed e.g. startup quite a bit).
    //
    // In order to avoid dex2oat from OOMing we set a hard limit on the
    // inflated size of a given debug program and instead of emitting one
    // single debug program for methods of arity A, we emit multiple debug
    // programs which are bucketed so that the inflated size of any single
    // debug program is smaller than what would be the inflated size of the
    // single mega-program shared by all methods.
    //
    // Max inflated count is 2^21 = 2M. Any bigger and the vector will grow to
    // 2^22 entries, any smaller and the vector will grow but not necessarily
    // be used. For now this has been arbitrarily been chosen.
    static constexpr size_t MAX_INFLATED_SIZE = 2 * 1024 * 1024;
    using Iter = DebugMethodMap::const_iterator;

    // Bucket the set of methods specified by begin, end into appropriately
    // sized buckets.
    // Returns a pair:
    // - A vector of {IODI size, method count} describing each bucket
    // - A size_t reflecting the total inflated footprint using the returned
    //   bucketing
    // If dry_run is specified then no allocations will be done and the vector
    // will be emptied (this is used to query for the total inflation size).
    auto create_buckets = [](Iter begin, Iter end, bool dry_run = false) {
      // In order to understand this algorithm let's first define what
      // the "inflated size" of an debug program is:
      //
      // The inflated size of a debug program D is the number of entries that
      // dex2oat will create in a vector when inflating debug info into IR. This
      // factor is computed as len(D) * ${number of methods using D}.
      //
      // Now, this function splits one large IODI program into multiple in order
      // to reduce the inflated size of each debug program. We must do this so
      // that dex2oat doesn't OOM. The algorithm proceeds as follows:
      //
      // - Define a max bucket size: MAX_BUCKET_INFLATED_SIZE. This is the limit
      //   on the inflated size of any given IODI debug program. We use this to
      //   determine how many buckets will be created.
      // - Since len(D) = max{ len(method) | method uses D } given D a debug
      //   program we can iterate from largest method to smallest, attempting
      //   to add the next smallest program into the current bucket and
      //   otherwise cutting the current bucket off. In pseudo code this is:
      //
      //   for method in methods, partially ordered from largest to smallest:
      //     if method can fit in current bucket:
      //       add method to current bucket
      //     else
      //       close up the current bucket and start a new one for method
      //
      //   There must be a precondition that the current bucket contains at
      //   least one method, otherwise we may run into empty buckets and
      //   silently ignored methods. We can prove that this by induction. First
      //   some terminology:
      //
      //   bucket_n := The nth bucket that has been created, starting at 0
      //   method_i := The ith largest method that's iterated over
      //
      //   Additionally we know that:
      //
      //   inflated_size(bucket_n) = max{ len(M) | M \in bucket_n }
      //                                  * len(bucket_n)
      //   and inflated_size(bucket_n) < MAX_BUCKET_INFLATED_SIZE
      //
      //   To establish the base case let's filter our set of methods to
      //     filtered_methods = { M \in methods
      //                            | len(methods) < MAX_BUCKET_INFLATED_SIZE }
      //   Now we have method_0 \in filtered_methods is such that
      //    len(method_0) < MAX_BUCKET_INFLATED_SIZE
      //   so bucket_0 can at least contain method_0 and thus is non-empty.
      //
      //   For the inductive case fix N to be the index of the current bucket
      //   and I to be the index of a method that cannot fit in the current
      //   bucket, then we know bucket_N is non-empty (by our inductive
      //   hypothesis) and thus, by above \exists M \in bucket_N exists s.t.
      //   len(M) < MAX_BUCKET_INFLATED_SIZE. We know that
      //   len(method_I) <= len(M) because the methods are partially ordered
      //   from largest to smallest and method_I comes after M. Thus we
      //   determine that len(method_I) <= len(M) < MAX_BUCKET_INFLATED_SIZE
      //   and so method_I can fit into bucket_{N+1}.
      //
      // No logic here, just picking 2^{some power} so that vectors don't
      // unnecessarily expand when inflating debug info for the current bucket.
      static constexpr size_t MAX_BUCKET_INFLATED_SIZE = 2 * 2 * 2 * 1024;
      std::vector<std::pair<uint32_t, uint32_t>> result;
      size_t total_inflated_footprint = 0;
      if (begin == end) {
        return std::make_pair(result, total_inflated_footprint);
      }
      uint32_t bucket_size = 0;
      uint32_t bucket_count = 0;
      auto append_bucket = [&](uint32_t size, uint32_t count) {
        total_inflated_footprint += size * count;
        if (!dry_run) {
          result.emplace_back(size, count);
        }
      };
      // To start we need to bucket any method that's too big for its own good
      // into its own bucket (this ensures the buckets calculated below contain
      // at least one entry).
      while (begin != end && begin->first.size > MAX_BUCKET_INFLATED_SIZE) {
        append_bucket(begin->first.size, 1);
        begin++;
      }
      for (auto iter = begin; iter != end; iter++) {
        uint32_t next_size = std::max(bucket_size, iter->first.size);
        uint32_t next_count = bucket_count + 1;
        size_t inflated_footprint = next_size * next_count;
        if (inflated_footprint > MAX_BUCKET_INFLATED_SIZE) {
          always_assert(bucket_size != 0 && bucket_count != 0);
          append_bucket(bucket_size, bucket_count);
          bucket_size = 0;
          bucket_count = 0;
        } else {
          bucket_size = next_size;
          bucket_count = next_count;
        }
      }
      if (bucket_size > 0 && bucket_count > 0) {
        append_bucket(bucket_size, bucket_count);
      }
      return std::make_pair(result, total_inflated_footprint);
    };

    auto compute = [&](const auto& sizes, bool dry_run) -> size_t {
      // The best size for us to start at is initialized as the largest method
      // This iterator will keep track of the smallest method that can use IODI.
      // If it points to end, then no method should use IODI.
      Iter best_iter = sizes.begin();
      Iter end = sizes.end();

      // Re-bucketing removing one method at a time until we've found a set of
      // methods small enough for the given constraints.
      size_t total_inflated_size = 0;
      do {
        total_inflated_size = create_buckets(best_iter, end, true).second;
      } while (total_inflated_size > MAX_INFLATED_SIZE && ++best_iter != end);
      size_t total_ignored = std::distance(sizes.begin(), best_iter);
      if (!dry_run) {
        TRACE(IODI, 3,
              "[IODI] (%u) Ignored %zu methods because they inflated too much",
              param_size, total_ignored);
      }

      // 2.1.2) In order to filter out methods who increase uncompressed APK
      // size we need to understand how IODI gets its win:
      //
      // The win is calculated as the total usual debug info size minus the size
      // of debug info when IODI is enabled. Thus, given a set of methods for
      // which IODI is enabled we have the following formula:
      //
      // win(IODI_methods) = normal_debug_size(all methods)
      //        - (IODI_debug_size(IODI_methods)
      //            + normal_debug_size(all_methods - IODI_methods))
      // where
      //  normal_debug_size(M) = the size of usual debug programs for all m in M
      //  IODI_debug_size(M) =
      //                      -----
      //                      \
      //                       \     max(len(m) + padding | m in M, arity(m) =
      //                       i)
      //                       /
      //                      /
      //                      -----
      //                  i in arities(M)
      //   or, in plain english, add together the size of a debug program for
      //   each arity i. Fixing an arity i, the size is calculated as the max
      //   length of a method with arity i with some constant padding added
      //   (the header of the dbg program)
      //
      // Simplifying the above a bit we get that:
      //
      // win(IM) =
      //          -----
      //          \
      //           \     normal_debug_size({ m in IM | arity(m) = i})
      //           /       - max(len(m) + padding | m in IM, arity(m) = i)
      //          /
      //          -----
      //      i in arities(IM)
      //
      // In order to maximize win we need to determine the best set of methods
      // that should use IODI (i.e. this is a maximization problem of win over
      // IM above). Since the summand above only depends on methods with arity
      // i, we can focus on maximizing the summand alone after fixing i. Thus we
      // need to maximize:
      //
      // win(IM) = normal_debug_size({ m in IM | arity(m) = i})
      //            - max(len(m) + padding | m in IM, arity(m) = i)
      //
      // It's clear that removing any method m s.t. len(m) < max(len(m) ...)
      // will make the overall win smaller, so our only chance is to remove the
      // biggest method. After removing the biggest method, or m_1, we get
      // a win delta of:
      //
      // win_delta_1 = len(m_1) - len(m_2) - normal_debug_size(m_1)
      // where m_2 is the next biggest method.
      //
      // We can continue to calculate more win_deltas if we were to remove the
      // subsequent biggest methods:
      //
      // win_delta_i = len(m_1) - len(m_{i+1})
      //                        - sum(j = 1, j < i, normal_debug_size(m_j))
      // or in other words, the delta of the iodi programs minus the cost of
      // incorporating all the normal debug programs up to i.
      //
      // Since there is no regularity condition on normal_debug_size(m) the
      // max of win_delta_i may occur for any i (indeed there may be an esoteric
      // case where all the debug programs are tiny but all the methods are
      // pretty large and thus it's best to not use any IODI programs).
      //
      // Note, the above assumes win(IM) > 0 at some point, but that may not be
      // true. In order to verify that using IODI is useful we need to verify
      // that win(IM) > 0 for whatever maximal IM is found was found above.
      auto iter = best_iter;
      // This is len(m_1) from above
      uint64_t base_iodi_size = iter->first.size;
      // This is that final sum in win_delta_i. It starts with just the debug
      // cost of m_1.
      uint64_t total_normal_dbg_cost = iter->second;
      // This keeps track of the best win delta. By default the delta is 0 (we
      // can always make everything use iodi)
      int64_t max_win_delta = 0;

      if (requires_iodi_programs) {
        for (iter = std::next(iter); iter != end; iter++) {
          uint64_t iodi_size = iter->first.size;
          // This is calculated as:
          //   "how much do we save by using a smaller iodi program after
          //    removing the cost of not using an iodi program for the larger
          //    methods"
          int64_t win_delta =
              (base_iodi_size - iodi_size) - total_normal_dbg_cost;
          // If it's as good as the win then we use it because we want to make
          // as small debug programs as possible due to dex2oat
          if (win_delta >= max_win_delta) {
            max_win_delta = win_delta;
            best_iter = iter;
          }
          total_normal_dbg_cost += iter->second;
        }
      }

      size_t insns_size = best_iter != end ? best_iter->first.size : 0;
      size_t padding = 1 + 1 + param_size + 1;
      if (param_size >= 128) {
        padding += 1;
        if (param_size >= 16384) {
          padding += 1;
        }
      }
      auto iodi_size = insns_size + padding;

      if (requires_iodi_programs) {
        if (total_normal_dbg_cost < iodi_size) {
          // If using IODI period isn't valuable then don't use it!
          best_iter = end;
          if (!dry_run) {
            TRACE(IODI, 3,
                  "[IODI] Opting out of IODI for %u arity methods entirely",
                  param_size);
          }
        }
      }

      // Now we've found which methods are too large to be beneficial. Tell IODI
      // infra about these large methods
      size_t num_big = 0;
      assert(sizes.begin() == best_iter || requires_iodi_programs);
      for (auto big = sizes.begin(); big != best_iter; big++) {
        if (!dry_run) {
          iodi_metadata.mark_method_huge(big->first.method);
          TRACE(IODI, 3,
                "[IODI] %s is too large to benefit from IODI: %u vs %u",
                SHOW(big->first.method), big->first.size, big->second);
        }
        num_big += 1;
      }

      size_t num_small_enough = sizes.size() - num_big;
      if (dry_run) {
        size_t sum = 0;
        for (auto it = sizes.begin(); it != best_iter; ++it) {
          sum += it->second;
        }
        // Does not include bucketing, but good enough.
        sum += num_small_enough * iodi_size;
        return sum;
      }

      // 2.2) Emit IODI programs (other debug programs will be emitted below)
      if (requires_iodi_programs) {
        TRACE(IODI, 2,
              "[IODI] @%u(%u): Of %zu methods %zu were too big, %zu at biggest "
              "%zu",
              offset, param_size, sizes.size(), num_big, num_small_enough,
              insns_size);
        if (num_small_enough == 0) {
          return 0;
        }
        auto bucket_res = create_buckets(best_iter, end);
        auto& buckets = bucket_res.first;
        total_inflated_size = bucket_res.second;
        TRACE(IODI, 3,
              "[IODI][Buckets] Bucketed %u arity methods into %zu buckets with "
              "total"
              " inflated size %zu:\n",
              param_size, buckets.size(), total_inflated_size);
        auto& size_to_offset = param_size_to_oset[param_size];
        for (auto& bucket : buckets) {
          auto bucket_size = bucket.first;
          TRACE(IODI, 3, "  - %u methods in bucket size %u @ %u", bucket.second,
                bucket_size, offset);
          size_to_offset.emplace(bucket_size, offset);
          std::vector<std::unique_ptr<DexDebugInstruction>> dbgops;
          if (bucket_size > 0) {
            // First emit an entry for pc = 0 -> line = start
            dbgops.push_back(DexDebugInstruction::create_line_entry(0, 0));
            // Now emit an entry for each pc thereafter
            // (0x1e increments addr+line by 1)
            for (size_t i = 1; i < bucket_size; i++) {
              dbgops.push_back(DexDebugInstruction::create_line_entry(1, 1));
            }
          }
          offset += DexDebugItem::encode(nullptr, output + offset, line_addin,
                                         param_size, dbgops);
          *dbgcount += 1;
        }
      }

      if (traceEnabled(IODI, 4)) {
        double ammortized_cost = 0;
        if (requires_iodi_programs) {
          ammortized_cost = (double)iodi_size / (double)num_small_enough;
        }
        for (auto it = best_iter; it != end; it++) {
          TRACE(IODI, 4,
                "[IODI][savings] %s saved %u bytes (%u), cost of %f, net %f",
                SHOW(it->first.method), it->second, it->first.size,
                ammortized_cost, (double)it->second - ammortized_cost);
        }
      }

      return 0;
    };
    auto mark_clusters_as_skip = [&](const auto& sizes) {
      // Mark methods in clusters as skip and remove them from param_to_sizes.
      for (const auto& p : sizes) {
        const auto* emitted_method = p.first.method;
        const auto* canonical =
            iodi_metadata.get_canonical_method(emitted_method);
        auto cluster_it = clustered_methods.find(canonical);
        if (cluster_it == clustered_methods.end()) {
          continue;
        }

        for (const auto* m : cluster_it->second) {
          if (m != emitted_method) {
            pso.skip(m);
            TRACE(IODI, 4, "Skipping %s for %s", SHOW(m), SHOW(emitted_method));
            auto& m_dbg = method_to_debug_meta.at(m);
            auto& param_methods = param_to_sizes.at(m_dbg.num_params);
            auto param_it = param_methods.find(MethodKey{m, m_dbg.dex_size});
            if (param_it != param_methods.end()) {
              param_methods.erase(param_it);
            }
          }
        }
      }
    };
    if (combinations == 1) {
      compute(dbg_sizes, /*dry_run=*/false);
      mark_clusters_as_skip(dbg_sizes);
    } else {
      auto sizes_wo_clusters = dbg_sizes;
      size_t max_cluster_len{0};
      size_t sum_cluster_sizes{0};
      for (auto& p : clusters_in_sizes) {
        for (const auto& k : p.second) {
          sizes_wo_clusters.erase(k);
        }
        std::sort(p.second.begin(), p.second.end(), MethodKeyCompare());
        max_cluster_len = std::max(max_cluster_len, p.second.size());
        for (const auto& k : p.second) {
          sum_cluster_sizes += dbg_sizes.at(k);
        }
      }
      TRACE(IODI, 3, "max_cluster_len=%zu sum_cluster_sizes=%zu",
            max_cluster_len, sum_cluster_sizes);

      // Very simple heuristic, "walk" in lock-step, do not try all combinations
      // (too expensive).
      size_t best_iter{0};
      size_t best_size{0};

      auto add_iteration = [&dbg_sizes, &clusters_in_sizes,
                            max_cluster_len](auto& cur_sizes, size_t iter) {
        size_t added_sizes{0};
        for (const auto& p : clusters_in_sizes) {
          size_t p_idx = p.second.size() -
                         std::min(p.second.size(), max_cluster_len - iter);
          const auto& k = p.second[p_idx];
          auto k_size = dbg_sizes.at(k);
          cur_sizes[k] = k_size;
          added_sizes += k_size;
        }
        return added_sizes;
      };

      for (size_t iter = 0; iter != max_cluster_len; ++iter) {
        auto cur_sizes = sizes_wo_clusters;
        auto added_sizes = add_iteration(cur_sizes, iter);

        auto out_size = compute(cur_sizes, /*dry_run=*/true) +
                        (sum_cluster_sizes - added_sizes);
        TRACE(IODI, 3,
              "Iteration %zu: added_sizes=%zu out_size=%zu extra_size=%zu",
              iter, added_sizes, out_size, sum_cluster_sizes - added_sizes);
        if (iter == 0) {
          best_size = out_size;
        } else if (out_size < best_size) {
          best_size = out_size;
          best_iter = iter;
        }
      }

      TRACE(IODI, 3, "Best iteration %zu (%zu)", best_iter, best_size);
      auto cur_sizes = sizes_wo_clusters;
      add_iteration(cur_sizes, best_iter);
      compute(cur_sizes, /*dry_run=*/false);
      mark_clusters_as_skip(cur_sizes);

      // Mark other cluster methods as skips.
      for (const auto& p : clusters_in_sizes) {
        size_t p_idx = p.second.size() -
                       std::min(p.second.size(), max_cluster_len - best_iter);
        for (size_t i = 0; i != p.second.size(); ++i) {
          if (i == p_idx) {
            continue;
          }
          pso.skip(p.second[i].method);
        }
      }
    }
  }

  auto post_iodi_offset = offset;
  TRACE(IODI, 2, "[IODI] IODI programs took up %d bytes\n",
        post_iodi_offset - initial_offset);
  // 3)
  auto size_offset_end = param_size_to_oset.end();
  std::unordered_set<const DexMethod*> to_remove;
  for (auto& it : code_items) {
    if (pso.skip_methods.count(it->method)) {
      continue;
    }

    DexCode* dc = it->code;
    const auto dbg = dc->get_debug_item();
    redex_assert(dbg != nullptr);
    auto code_size = dc->size();
    redex_assert(code_size != 0);
    // If a method is too big then it's been marked as so internally, so this
    // will return false.
    DexMethod* method = it->method;
    if (!iodi_metadata.is_huge(method)) {
      iodi_metadata.set_iodi_layer(method, iodi_layer);
      TRACE(IODI, 3, "Emitting %s as IODI", SHOW(method));
      if (requires_iodi_programs) {
        // Here we sanity check to make sure that all IODI programs are at least
        // as long as they need to be.
        uint32_t param_size = it->method->get_proto()->get_args()->size();
        auto size_offset_it = param_size_to_oset.find(param_size);
        always_assert_log(size_offset_it != size_offset_end,
                          "Expected to find param to offset: %s", SHOW(method));
        auto& size_to_offset = size_offset_it->second;
        // Returns first key >= code_size or end if such an entry doesn't exist.
        // Aka first debug program long enough to represent a method of size
        // code_size.
        auto offset_it = size_to_offset.lower_bound(code_size);
        auto offset_end = size_to_offset.end();
        always_assert_log(offset_it != offset_end,
                          "Expected IODI program to be big enough for %s : %u",
                          SHOW(method), code_size);
        it->code_item->debug_info_off = offset_it->second;
      } else {
        it->code_item->debug_info_off = 0;
      }
    } else {
      TRACE(IODI, 3, "Emitting %s as non-IODI", SHOW(method));
      // Recompute the debug data with no line add-in if not in a cluster.
      // TODO: If a whole cluster does not have IODI, we should emit base
      //       versions for all of them.
      DebugMetadata no_line_addin_metadata;
      const DebugMetadata* metadata = &method_to_debug_meta.at(method);
      if (!is_in_global_cluster(method) && line_addin != 0) {
        no_line_addin_metadata =
            calculate_debug_metadata(dbg, dc, it->code_item, pos_mapper,
                                     metadata->num_params, code_debug_map,
                                     /*line_addin=*/0);
        metadata = &no_line_addin_metadata;
      }
      offset +=
          emit_debug_info_for_metadata(dodx, *metadata, output, offset, true);
      *dbgcount += 1;
    }
    to_remove.insert(method);
  }
  code_items.erase(std::remove_if(code_items.begin(), code_items.end(),
                                  [&to_remove](const CodeItemEmit* cie) {
                                    return to_remove.count(cie->method) > 0;
                                  }),
                   code_items.end());
  TRACE(IODI, 2, "[IODI] Non-IODI programs took up %d bytes\n",
        offset - post_iodi_offset);
  // Return how much data we've encoded
  return offset - initial_offset;
}

uint32_t emit_instruction_offset_debug_info(
    DexOutputIdx* dodx,
    PositionMapper* pos_mapper,
    std::vector<CodeItemEmit>& code_items,
    IODIMetadata& iodi_metadata,
    bool iodi_layers,
    size_t store_number,
    size_t dex_number,
    uint8_t* output,
    uint32_t offset,
    int* dbgcount,
    std::unordered_map<DexCode*, std::vector<DebugLineItem>>* code_debug_map) {
  // IODI only supports non-ambiguous methods, i.e., an overload cluster is
  // only a single method. Layered IODI supports as many overloads as can
  // be encoded.
  const size_t large_bound = iodi_layers ? DexOutput::kIODILayerBound : 1;

  std::unordered_set<const DexMethod*> too_large_cluster_methods;
  {
    for (const auto& p : iodi_metadata.get_name_clusters()) {
      if (p.second.size() > large_bound) {
        too_large_cluster_methods.insert(p.second.begin(), p.second.end());
      }
    }
  }
  TRACE(IODI, 1, "%zu methods in too-large clusters.",
        too_large_cluster_methods.size());

  std::vector<CodeItemEmit*> code_items_tmp;
  code_items_tmp.reserve(code_items.size());
  std::transform(code_items.begin(), code_items.end(),
                 std::back_inserter(code_items_tmp),
                 [](CodeItemEmit& cie) { return &cie; });
  // Remove all items without debug info or no code.
  code_items_tmp.erase(std::remove_if(code_items_tmp.begin(),
                                      code_items_tmp.end(),
                                      [](auto cie) {
                                        if (!cie->code->get_debug_item()) {
                                          return true;
                                        };
                                        if (cie->code->size() == 0) {
                                          // If there are no instructions then
                                          // we don't need any debug info!
                                          cie->code_item->debug_info_off = 0;
                                          return true;
                                        }
                                        return false;
                                      }),
                       code_items_tmp.end());
  TRACE(IODI, 1, "Removed %zu CIEs w/o debug data.",
        code_items.size() - code_items_tmp.size());
  // Remove all unsupported items.
  std::vector<CodeItemEmit*> unsupported_code_items;
  if (!too_large_cluster_methods.empty()) {
    code_items_tmp.erase(
        std::remove_if(code_items_tmp.begin(), code_items_tmp.end(),
                       [&](CodeItemEmit* cie) {
                         bool supported =
                             too_large_cluster_methods.count(cie->method) == 0;
                         if (!supported) {
                           iodi_metadata.mark_method_huge(cie->method);
                           unsupported_code_items.push_back(cie);
                         }
                         return !supported;
                       }),
        code_items_tmp.end());
  }

  const uint32_t initial_offset = offset;
  if (!code_items_tmp.empty()) {
    for (size_t i = 0; i < large_bound; ++i) {
      if (code_items_tmp.empty()) {
        break;
      }
      TRACE(IODI, 1, "IODI iteration %zu", i);
      const size_t before_size = code_items_tmp.size();
      offset += emit_instruction_offset_debug_info(
          dodx, pos_mapper, code_items_tmp, iodi_metadata, i,
          i << DexOutput::kIODILayerShift, store_number, dex_number, output,
          offset, dbgcount, code_debug_map);
      const size_t after_size = code_items_tmp.size();
      redex_assert(after_size < before_size);
    }
  }
  redex_assert(code_items_tmp.empty());

  // Emit the methods we could not handle.
  for (auto* cie : unsupported_code_items) {
    DexCode* dc = cie->code;
    redex_assert(dc->size() != 0);
    const auto dbg_item = dc->get_debug_item();
    redex_assert(dbg_item);
    DexMethod* method = cie->method;
    uint32_t param_size = method->get_proto()->get_args()->size();
    DebugMetadata metadata =
        calculate_debug_metadata(dbg_item, dc, cie->code_item, pos_mapper,
                                 param_size, code_debug_map, /*line_addin=*/0);
    offset +=
        emit_debug_info_for_metadata(dodx, metadata, output, offset, true);
    *dbgcount += 1;
    iodi_metadata.mark_method_huge(method);
  }

  // Return how much data we've encoded
  return offset - initial_offset;
}

} // namespace

void DexOutput::generate_debug_items() {
  uint32_t dbg_start = m_offset;
  int dbgcount = 0;
  bool emit_positions = m_debug_info_kind != DebugInfoKind::NoPositions;
  bool use_iodi = is_iodi(m_debug_info_kind);
  if (use_iodi && m_iodi_metadata) {
    inc_offset(emit_instruction_offset_debug_info(
        m_dodx.get(),
        m_pos_mapper,
        m_code_item_emits,
        *m_iodi_metadata,
        m_debug_info_kind == DebugInfoKind::InstructionOffsetsLayered,
        m_store_number,
        m_dex_number,
        m_output.get(),
        m_offset,
        &dbgcount,
        m_code_debug_lines));
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
      size_t num_params = it.method->get_proto()->get_args()->size();
      inc_offset(emit_debug_info(m_dodx.get(), emit_positions, dbg, dc, dci,
                                 m_pos_mapper, m_output.get(), m_offset,
                                 num_params, m_code_debug_lines));
    }
  }
  if (emit_positions) {
    insert_map_item(TYPE_DEBUG_INFO_ITEM, dbgcount, dbg_start,
                    m_offset - dbg_start);
  }
  m_stats.num_dbg_items += dbgcount;
  m_stats.dbg_total_size += m_offset - dbg_start;
}

void DexOutput::generate_map() {
  align_output();
  uint32_t* mapout = (uint32_t*)(m_output.get() + m_offset);
  hdr.map_off = m_offset;
  insert_map_item(TYPE_MAP_LIST, 1, m_offset,
                  sizeof(uint32_t) + m_map_items.size() * sizeof(dex_map_item));
  *mapout = (uint32_t)m_map_items.size();
  dex_map_item* map = (dex_map_item*)(mapout + 1);
  for (auto const& mit : m_map_items) {
    *map++ = mit;
  }
  inc_offset(((uint8_t*)map) - ((uint8_t*)mapout));
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

void DexOutput::init_header_offsets(const std::string& dex_magic) {
  always_assert_log(dex_magic.length() > 0,
                    "Invalid dex magic from input APK\n");
  memcpy(hdr.magic, dex_magic.c_str(), sizeof(hdr.magic));
  uint32_t total_hdr_size = sizeof(dex_header);
  insert_map_item(TYPE_HEADER_ITEM, 1, 0, total_hdr_size);

  m_offset = hdr.header_size = total_hdr_size;
  hdr.endian_tag = ENDIAN_CONSTANT;
  /* Link section was never used */
  hdr.link_size = hdr.link_off = 0;
  hdr.string_ids_size = (uint32_t)m_dodx->stringsize();
  hdr.string_ids_off = hdr.string_ids_size ? m_offset : 0;
  uint32_t total_string_size = m_dodx->stringsize() * sizeof(dex_string_id);
  insert_map_item(TYPE_STRING_ID_ITEM, (uint32_t)m_dodx->stringsize(), m_offset,
                  total_string_size);

  inc_offset(total_string_size);
  hdr.type_ids_size = (uint32_t)m_dodx->typesize();
  hdr.type_ids_off = hdr.type_ids_size ? m_offset : 0;
  uint32_t total_type_size = m_dodx->typesize() * sizeof(dex_type_id);
  insert_map_item(TYPE_TYPE_ID_ITEM, (uint32_t)m_dodx->typesize(), m_offset,
                  total_type_size);

  inc_offset(total_type_size);
  hdr.proto_ids_size = (uint32_t)m_dodx->protosize();
  hdr.proto_ids_off = hdr.proto_ids_size ? m_offset : 0;
  uint32_t total_proto_size = m_dodx->protosize() * sizeof(dex_proto_id);
  insert_map_item(TYPE_PROTO_ID_ITEM, (uint32_t)m_dodx->protosize(), m_offset,
                  total_proto_size);

  inc_offset(total_proto_size);
  hdr.field_ids_size = (uint32_t)m_dodx->fieldsize();
  hdr.field_ids_off = hdr.field_ids_size ? m_offset : 0;
  uint32_t total_field_size = m_dodx->fieldsize() * sizeof(dex_field_id);
  insert_map_item(TYPE_FIELD_ID_ITEM, (uint32_t)m_dodx->fieldsize(), m_offset,
                  total_field_size);

  inc_offset(total_field_size);
  hdr.method_ids_size = (uint32_t)m_dodx->methodsize();
  hdr.method_ids_off = hdr.method_ids_size ? m_offset : 0;
  uint32_t total_method_size = m_dodx->methodsize() * sizeof(dex_method_id);
  insert_map_item(TYPE_METHOD_ID_ITEM, (uint32_t)m_dodx->methodsize(), m_offset,
                  total_method_size);

  inc_offset(total_method_size);
  hdr.class_defs_size = (uint32_t)m_classes->size();
  hdr.class_defs_off = hdr.class_defs_size ? m_offset : 0;
  uint32_t total_class_size = m_classes->size() * sizeof(dex_class_def);
  insert_map_item(TYPE_CLASS_DEF_ITEM, (uint32_t)m_classes->size(), m_offset,
                  total_class_size);

  inc_offset(total_class_size);

  uint32_t total_callsite_size =
      m_dodx->callsitesize() * sizeof(dex_callsite_id);
  insert_map_item(TYPE_CALL_SITE_ID_ITEM, (uint32_t)m_dodx->callsitesize(),
                  m_offset, total_callsite_size);
  inc_offset(total_callsite_size);

  uint32_t total_methodhandle_size =
      m_dodx->methodhandlesize() * sizeof(dex_methodhandle_id);
  insert_map_item(TYPE_METHOD_HANDLE_ITEM, (uint32_t)m_dodx->methodhandlesize(),
                  m_offset, total_methodhandle_size);
  inc_offset(total_methodhandle_size);

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
  memcpy(m_output.get(), &hdr, sizeof(hdr));
  Sha1Context context;
  sha1_init(&context);
  sha1_update(&context, m_output.get() + skip, hdr.file_size - skip);
  sha1_final(hdr.signature, &context);
  memcpy(m_output.get(), &hdr, sizeof(hdr));
  uint32_t adler = (uint32_t)adler32(0L, Z_NULL, 0);
  skip = sizeof(hdr.magic) + sizeof(hdr.checksum);
  adler = (uint32_t)adler32(adler, (const Bytef*)(m_output.get() + skip),
                            hdr.file_size - skip);
  hdr.checksum = adler;
  memcpy(m_output.get(), &hdr, sizeof(hdr));
}

namespace {

void compute_method_to_id_map(
    DexOutputIdx* dodx,
    const DexClasses* classes,
    uint8_t* dex_signature,
    std::unordered_map<DexMethod*, uint64_t>* method_to_id) {
  if (!method_to_id) {
    return;
  }

  std::unordered_set<DexClass*> dex_classes(classes->begin(), classes->end());
  for (auto& it : dodx->method_to_idx()) {
    auto method = it.first;
    auto idx = it.second;

    auto const& typecls = method->get_class();
    auto const& cls = type_class(typecls);
    if (dex_classes.count(cls) == 0) {
      continue;
    }

    auto resolved_method = [&]() -> DexMethodRef* {
      if (cls) {
        auto resm = resolve_method(method,
                                   is_interface(cls) ? MethodSearch::Interface
                                                     : MethodSearch::Any);
        if (resm) return resm;
      }
      return method;
    }();

    // Turns out, the checksum can change on-device. (damn you dexopt)
    // The signature, however, is never recomputed. Let's log the top 4 bytes,
    // in little-endian (since that's faster to compute on-device).
    uint32_t signature = *reinterpret_cast<uint32_t*>(dex_signature);

    if (resolved_method == method) {
      // Not recording it if method reference is not referring to
      // concrete method, otherwise will have key overlapped.
      auto dexmethod = static_cast<DexMethod*>(resolved_method);
      (*method_to_id)[dexmethod] = ((uint64_t)idx << 32) | (uint64_t)signature;
    }
  }
}

void write_method_mapping(const std::string& filename,
                          const DexOutputIdx* dodx,
                          const DexClasses* classes,
                          uint8_t* dex_signature) {
  always_assert(!filename.empty());
  FILE* fd = fopen(filename.c_str(), "a");
  assert_log(fd, "Can't open method mapping file %s: %s\n", filename.c_str(),
             strerror(errno));
  std::unordered_set<DexClass*> classes_in_dex(classes->begin(),
                                               classes->end());
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
                                   is_interface(cls) ? MethodSearch::Interface
                                                     : MethodSearch::Any);
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
    auto deobf_method_name = deobf_method.substr(begin, end - begin);

    // Turns out, the checksum can change on-device. (damn you dexopt)
    // The signature, however, is never recomputed. Let's log the top 4 bytes,
    // in little-endian (since that's faster to compute on-device).
    uint32_t signature = *reinterpret_cast<uint32_t*>(dex_signature);

    fprintf(fd, "%u %u %s %s\n", idx, signature, deobf_method_name.c_str(),
            deobf_class.c_str());
  }
  fclose(fd);
}

void write_class_mapping(const std::string& filename,
                         DexClasses* classes,
                         const size_t class_defs_size,
                         uint8_t* dex_signature) {
  always_assert(!filename.empty());
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
    not_reached_log("Illegal type: %c", type);
  }
}

void write_pg_mapping(
    const std::string& filename,
    DexClasses* classes,
    const std::unordered_map<DexClass*, std::vector<DexMethod*>>*
        detached_methods) {
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
      if (type::is_array(type)) {
        auto* type_str = type->c_str();
        int dim = 0;
        while (type_str[dim] == '[') {
          dim++;
        }
        DexType* inner_type = DexType::get_type(&type_str[dim]);
        DexClass* inner_cls = inner_type ? type_class(inner_type) : nullptr;
        std::string result;
        if (inner_cls) {
          result = java_names::internal_to_external(deobf_class(inner_cls));
        } else if (inner_type && type::is_primitive(inner_type)) {
          result = deobf_primitive(type_str[dim]);
        } else {
          result = java_names::internal_to_external(&type_str[dim]);
        }
        for (int i = 0; i < dim; ++i) {
          result = result + "[]";
        }
        return result;
      } else {
        DexClass* cls = type_class(type);
        if (cls) {
          return java_names::internal_to_external(deobf_class(cls));
        } else if (type::is_primitive(type)) {
          return std::string(deobf_primitive(type->c_str()[0]));
        } else {
          return java_names::internal_to_external(type->c_str());
        }
      }
    }
    return show(type);
  };

  auto deobf_meth = [&](DexMethod* method) {
    if (method) {
      /* clang-format off */
      // Example: 672:672:boolean customShouldDelayInitMessage(android.os.Handler,android.os.Message)
      /* clang-format on */
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
        if (line_start >
            static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
          line_start = 0;
        }
        if (line_end >
            static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
          line_end = 0;
        }
        ss << line_start << ":" << line_end << ":";
      }
      auto* rtype = proto->get_rtype();
      auto rtype_str = deobf_type(rtype);
      ss << rtype_str;
      ss << " " << method->get_simple_deobfuscated_name() << "(";
      auto args = proto->get_args()->get_type_list();
      for (auto iter = args.begin(); iter != args.end(); ++iter) {
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
      ss << deobf_type(field->get_type()) << " "
         << field->get_simple_deobfuscated_name();
      return ss.str();
    }
    return show(field);
  };

  std::ofstream ofs(filename.c_str(), std::ofstream::out | std::ofstream::app);

  for (auto cls : *classes) {
    auto deobf_cls = deobf_class(cls);
    ofs << java_names::internal_to_external(deobf_cls) << " -> "
        << java_names::internal_to_external(cls->get_type()->c_str()) << ":"
        << std::endl;
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
    if (detached_methods) {
      auto it = detached_methods->find(cls);
      if (it != detached_methods->end()) {
        ofs << "    --- detached methods ---" << std::endl;
        for (auto meth : it->second) {
          auto deobf = deobf_meth(meth);
          ofs << "    " << deobf << " -> " << meth->c_str() << std::endl;
        }
      }
    }
  }
}

void write_full_mapping(const std::string& filename, DexClasses* classes) {
  if (filename.empty()) return;

  std::ofstream ofs(filename.c_str(), std::ofstream::out | std::ofstream::app);
  for (auto cls : *classes) {
    ofs << "type " << cls->get_deobfuscated_name() << " -> " << show(cls)
        << std::endl;
    for (auto field : cls->get_ifields()) {
      ofs << "ifield " << field->get_deobfuscated_name() << " -> "
          << show(field) << std::endl;
    }
    for (auto field : cls->get_sfields()) {
      ofs << "sfield " << field->get_deobfuscated_name() << " -> "
          << show(field) << std::endl;
    }
    for (auto method : cls->get_dmethods()) {
      ofs << "dmethod " << method->get_deobfuscated_name() << " -> "
          << show(method) << std::endl;
    }
    for (auto method : cls->get_vmethods()) {
      ofs << "vmethod " << method->get_deobfuscated_name() << " -> "
          << show(method) << std::endl;
    }
  }
}

void write_bytecode_offset_mapping(
    const std::string& filename,
    const std::vector<std::pair<std::string, uint32_t>>& method_offsets) {
  if (filename.empty()) {
    return;
  }

  auto fd = fopen(filename.c_str(), "a");
  assert_log(fd, "Can't open bytecode offset file %s: %s\n", filename.c_str(),
             strerror(errno));

  for (const auto& item : method_offsets) {
    fprintf(fd, "%u %s\n", item.second, item.first.c_str());
  }

  fclose(fd);
}

} // namespace

void DexOutput::write_symbol_files() {
  if (m_debug_info_kind != DebugInfoKind::NoCustomSymbolication) {
    write_method_mapping(m_method_mapping_filename, m_dodx.get(), m_classes,
                         hdr.signature);
    write_class_mapping(m_class_mapping_filename, m_classes,
                        hdr.class_defs_size, hdr.signature);
    // XXX: should write_bytecode_offset_mapping be included here too?
  }
  write_pg_mapping(m_pg_mapping_filename, m_classes, &m_detached_methods);
  write_full_mapping(m_full_mapping_filename, m_classes);
  write_bytecode_offset_mapping(m_bytecode_offset_filename,
                                m_method_bytecode_offsets);
}

void GatheredTypes::set_config(ConfigFiles* config) { m_config = config; }

void DexOutput::prepare(SortMode string_mode,
                        const std::vector<SortMode>& code_mode,
                        ConfigFiles& conf,
                        const std::string& dex_magic) {
  m_gtypes->set_config(&conf);

  fix_jumbos(m_classes, m_dodx.get());
  init_header_offsets(dex_magic);
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
  generate_callsite_data();
  generate_methodhandle_data();
  generate_annotations();
  generate_debug_items();
  generate_map();
  finalize_header();
  compute_method_to_id_map(m_dodx.get(), m_classes, hdr.signature,
                           m_method_to_id);
}

void DexOutput::write() {
  struct stat st;
  int fd = open(m_filename, O_CREAT | O_TRUNC | O_WRONLY | O_BINARY, 0660);
  if (fd == -1) {
    perror("Error writing dex");
    return;
  }
  ::write(fd, m_output.get(), m_offset);
  if (0 == fstat(fd, &st)) {
    m_stats.num_bytes = st.st_size;
  }
  close(fd);

  write_symbol_files();
}

class UniqueReferences {
 public:
  std::unordered_set<DexString*> strings;
  std::unordered_set<DexType*> types;
  std::unordered_set<DexProto*> protos;
  std::unordered_set<DexFieldRef*> fields;
  std::unordered_set<DexMethodRef*> methods;
  int total_strings_size{0};
  int total_types_size{0};
  int total_protos_size{0};
  int total_fields_size{0};
  int total_methods_size{0};
  int dexes{0};
};
UniqueReferences s_unique_references;

void DexOutput::metrics() {
  if (s_unique_references.dexes++ == 1 && !m_normal_primary_dex) {
    // clear out info from first (primary) dex
    s_unique_references.strings.clear();
    s_unique_references.types.clear();
    s_unique_references.protos.clear();
    s_unique_references.fields.clear();
    s_unique_references.methods.clear();
    s_unique_references.total_strings_size = 0;
    s_unique_references.total_types_size = 0;
    s_unique_references.total_protos_size = 0;
    s_unique_references.total_fields_size = 0;
    s_unique_references.total_methods_size = 0;
  }
  memcpy(m_stats.signature, hdr.signature, 20);

  for (auto& p : m_dodx->string_to_idx()) {
    s_unique_references.strings.insert(p.first);
  }
  m_stats.num_unique_strings = s_unique_references.strings.size();
  s_unique_references.total_strings_size += m_dodx->string_to_idx().size();
  m_stats.strings_total_size = s_unique_references.total_strings_size;

  for (auto& p : m_dodx->type_to_idx()) {
    s_unique_references.types.insert(p.first);
  }
  m_stats.num_unique_types = s_unique_references.types.size();
  s_unique_references.total_types_size += m_dodx->type_to_idx().size();
  m_stats.types_total_size = s_unique_references.total_types_size;

  for (auto& p : m_dodx->proto_to_idx()) {
    s_unique_references.protos.insert(p.first);
  }
  m_stats.num_unique_protos = s_unique_references.protos.size();
  s_unique_references.total_protos_size += m_dodx->proto_to_idx().size();
  m_stats.protos_total_size = s_unique_references.total_protos_size;

  for (auto& p : m_dodx->field_to_idx()) {
    s_unique_references.fields.insert(p.first);
  }
  m_stats.num_unique_field_refs = s_unique_references.fields.size();
  s_unique_references.total_fields_size += m_dodx->field_to_idx().size();
  m_stats.field_refs_total_size = s_unique_references.total_fields_size;

  for (auto& p : m_dodx->method_to_idx()) {
    s_unique_references.methods.insert(p.first);
  }
  m_stats.num_unique_method_refs = s_unique_references.methods.size();
  s_unique_references.total_methods_size += m_dodx->method_to_idx().size();
  m_stats.method_refs_total_size = s_unique_references.total_methods_size;
}

static SortMode make_sort_bytecode(const std::string& sort_bytecode) {
  if (sort_bytecode == "class_order") {
    return SortMode::CLASS_ORDER;
  } else if (sort_bytecode == "clinit_order") {
    return SortMode::CLINIT_FIRST;
  } else if (sort_bytecode == "method_profiled_order") {
    return SortMode::METHOD_PROFILED_ORDER;
  } else if (sort_bytecode == "method_similarity_order") {
    return SortMode::METHOD_SIMILARITY;
  } else {
    return SortMode::DEFAULT;
  }
}

dex_stats_t write_classes_to_dex(
    const RedexOptions& redex_options,
    const std::string& filename,
    DexClasses* classes,
    std::shared_ptr<GatheredTypes> gtypes,
    LocatorIndex* locator_index,
    size_t store_number,
    size_t dex_number,
    ConfigFiles& conf,
    PositionMapper* pos_mapper,
    std::unordered_map<DexMethod*, uint64_t>* method_to_id,
    std::unordered_map<DexCode*, std::vector<DebugLineItem>>* code_debug_lines,
    IODIMetadata* iodi_metadata,
    const std::string& dex_magic,
    PostLowering* post_lowering,
    int min_sdk) {
  const JsonWrapper& json_cfg = conf.get_json_config();
  bool force_single_dex = json_cfg.get("force_single_dex", false);
  if (force_single_dex) {
    always_assert_log(dex_number == 0, "force_single_dex requires one dex");
  }
  auto sort_strings = json_cfg.get("string_sort_mode", std::string());
  SortMode string_sort_mode = SortMode::DEFAULT;
  if (sort_strings == "class_strings") {
    string_sort_mode = SortMode::CLASS_STRINGS;
  } else if (sort_strings == "class_order") {
    string_sort_mode = SortMode::CLASS_ORDER;
  }

  auto interdex_config = json_cfg.get("InterDexPass", Json::Value());
  auto normal_primary_dex =
      interdex_config.get("normal_primary_dex", false).asBool();
  auto sort_bytecode_cfg = json_cfg.get("bytecode_sort_mode", Json::Value());
  std::vector<SortMode> code_sort_mode;

  if (sort_bytecode_cfg.isString()) {
    code_sort_mode.push_back(make_sort_bytecode(sort_bytecode_cfg.asString()));
  } else if (sort_bytecode_cfg.isArray()) {
    for (const auto& val : sort_bytecode_cfg) {
      code_sort_mode.push_back(make_sort_bytecode(val.asString()));
    }
  }
  auto disable_method_similarity_order =
      json_cfg.get("disable_method_similarity_order", false);
  if (disable_method_similarity_order) {
    TRACE(OPUT, 3, "[write_classes_to_dex] disable_method_similarity_order");
    code_sort_mode.erase(
        std::remove_if(
            code_sort_mode.begin(), code_sort_mode.end(),
            [&](SortMode sm) { return sm == SortMode::METHOD_SIMILARITY; }),
        code_sort_mode.end());
  }
  if (code_sort_mode.empty()) {
    code_sort_mode.push_back(SortMode::DEFAULT);
  }

  TRACE(OPUT, 2, "[write_classes_to_dex][filename] %s", filename.c_str());

  DexOutput dout(filename.c_str(), classes, std::move(gtypes), locator_index,
                 normal_primary_dex, store_number, dex_number,
                 redex_options.debug_info_kind, iodi_metadata, conf, pos_mapper,
                 method_to_id, code_debug_lines, post_lowering, min_sdk);

  dout.prepare(string_sort_mode, code_sort_mode, conf, dex_magic);
  dout.write();
  dout.metrics();
  return dout.m_stats;
}

LocatorIndex make_locator_index(DexStoresVector& stores) {
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
        const auto cstr = clsname->c_str();
        uint32_t global_clsnr = Locator::decodeGlobalClassIndex(cstr);
        if (global_clsnr != Locator::invalid_global_class_index) {
          TRACE(LOC, 3,
                "%s (%u, %u, %u) needs no locator; global class index=%u", cstr,
                strnr, dexnr, clsnr, global_clsnr);
          // This prefix is followed by the global class index; this case
          // doesn't need a locator.
          continue;
        }

        bool inserted = index
                            .insert(std::make_pair(
                                clsname, Locator::make(strnr, dexnr, clsnr)))
                            .second;
        // We shouldn't see the same class defined in two dexen
        always_assert_log(inserted, "This was already inserted %s\n",
                          (*clsit)->get_deobfuscated_name().c_str());
        (void)inserted; // Shut up compiler when defined(NDEBUG)
      }
    }
  }

  return index;
}

void DexOutput::inc_offset(uint32_t v) {
  // If this asserts hits, we already wrote out of bounds.
  always_assert(m_offset + v < m_output_size);
  // If this assert hits, we are too close.
  always_assert_log(
      m_offset + v < m_output_size - k_output_red_zone,
      "Running into output safety margin: %u of %zu(%zu). Increase the buffer "
      "size with `-J dex_output_buffer_size=`.",
      m_offset + v, m_output_size - k_output_red_zone, m_output_size);
  m_offset += v;
}
