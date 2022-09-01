/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexLoader.h"

#include "AggregateException.h"
#include "DexAccess.h"
#include "DexCallSite.h"
#include "DexDefs.h"
#include "DexMethodHandle.h"
#include "IRCode.h"
#include "Macros.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"
#include "WorkQueue.h"

#include <exception>
#include <stdexcept>
#include <vector>

DexLoader::DexLoader(const DexLocation* location)
    : m_idx(nullptr),
      m_file(new boost::iostreams::mapped_file()),
      m_location(location) {}

static void validate_dex_header(const dex_header* dh,
                                size_t dexsize,
                                int support_dex_version) {
  bool supported = false;
  switch (support_dex_version) {
  case 38:
    supported = supported ||
                !memcmp(dh->magic, DEX_HEADER_DEXMAGIC_V38, sizeof(dh->magic));
    FALLTHROUGH_INTENDED; /* intentional fallthrough to also check for v37 */
  case 37:
    supported = supported ||
                !memcmp(dh->magic, DEX_HEADER_DEXMAGIC_V37, sizeof(dh->magic));
    FALLTHROUGH_INTENDED; /* intentional fallthrough to also check for v35 */
  case 35:
    supported = supported ||
                !memcmp(dh->magic, DEX_HEADER_DEXMAGIC_V35, sizeof(dh->magic));
    break;
  default:
    not_reached_log("Unrecognized support_dex_version %d\n",
                    support_dex_version);
  }
  always_assert_log(supported, "Bad dex magic %s for support_dex_version %d\n",
                    dh->magic, support_dex_version);
  always_assert_log(
      dh->file_size == dexsize,
      "Reported size in header (%zu) does not match file size (%u)\n",
      dexsize,
      dh->file_size);
  auto off = (uint64_t)dh->class_defs_off;
  auto limit = off + dh->class_defs_size * sizeof(dex_class_def);
  always_assert_log(off < dexsize, "class_defs_off out of range");
  always_assert_log(limit <= dexsize, "invalid class_defs_size");
}

void DexLoader::gather_input_stats(dex_stats_t* stats, const dex_header* dh) {
  if (!stats) {
    return;
  }
  stats->num_types += dh->type_ids_size;
  stats->num_classes += dh->class_defs_size;
  stats->num_method_refs += dh->method_ids_size;
  stats->num_field_refs += dh->field_ids_size;
  stats->num_strings += dh->string_ids_size;
  stats->num_protos += dh->proto_ids_size;
  stats->num_bytes += dh->file_size;
  // T58562665: TODO - actually update states for callsites/methodhandles
  stats->num_callsites += 0;
  stats->num_methodhandles += 0;

  std::unordered_set<DexEncodedValueArray, boost::hash<DexEncodedValueArray>>
      enc_arrays;
  std::set<DexTypeList*, dextypelists_comparator> type_lists;
  std::unordered_set<uint32_t> anno_offsets;

  for (uint32_t cidx = 0; cidx < dh->class_defs_size; ++cidx) {
    auto* clz = m_classes->at(cidx);
    if (clz == nullptr) {
      // Skip nulls, they may have been introduced by benign duplicate classes
      continue;
    }
    auto* class_def = &m_class_defs[cidx];
    auto anno_off = class_def->annotations_off;
    if (anno_off) {
      const dex_annotations_directory_item* anno_dir =
          (const dex_annotations_directory_item*)m_idx->get_uint_data(anno_off);
      auto class_anno_off = anno_dir->class_annotations_off;
      if (class_anno_off) {
        const uint32_t* anno_data = m_idx->get_uint_data(class_anno_off);
        uint32_t count = *anno_data++;
        for (uint32_t aidx = 0; aidx < count; ++aidx) {
          anno_offsets.insert(anno_data[aidx]);
        }
      }
      const uint32_t* anno_data = (uint32_t*)(anno_dir + 1);
      for (uint32_t fidx = 0; fidx < anno_dir->fields_size; ++fidx) {
        anno_data++;
        anno_offsets.insert(*anno_data++);
      }
      for (uint32_t midx = 0; midx < anno_dir->methods_size; ++midx) {
        anno_data++;
        anno_offsets.insert(*anno_data++);
      }
      for (uint32_t pidx = 0; pidx < anno_dir->parameters_size; ++pidx) {
        anno_data++;
        uint32_t xrefoff = *anno_data++;
        if (xrefoff != 0) {
          const uint32_t* annoxref = m_idx->get_uint_data(xrefoff);
          uint32_t count = *annoxref++;
          for (uint32_t j = 0; j < count; j++) {
            uint32_t off = annoxref[j];
            anno_offsets.insert(off);
          }
        }
      }
    }
    auto* interfaces_type_list = clz->get_interfaces();
    type_lists.insert(interfaces_type_list);
    auto deva = clz->get_static_values();
    if (deva) {
      if (!enc_arrays.count(*deva)) {
        enc_arrays.emplace(std::move(*deva));
        stats->num_static_values++;
      }
    }
    stats->num_fields += clz->get_ifields().size() + clz->get_sfields().size();
    stats->num_methods +=
        clz->get_vmethods().size() + clz->get_dmethods().size();
    for (auto* meth : clz->get_vmethods()) {
      DexCode* code = meth->get_dex_code();
      if (code) {
        stats->num_instructions += code->get_instructions().size();
      }
    }
    for (auto* meth : clz->get_dmethods()) {
      DexCode* code = meth->get_dex_code();
      if (code) {
        stats->num_instructions += code->get_instructions().size();
      }
    }
  }
  for (uint32_t meth_idx = 0; meth_idx < dh->method_ids_size; ++meth_idx) {
    auto* meth = m_idx->get_methodidx(meth_idx);
    DexProto* proto = meth->get_proto();
    type_lists.insert(proto->get_args());
  }
  stats->num_annotations += anno_offsets.size();
  stats->num_type_lists += type_lists.size();

  for (uint32_t sidx = 0; sidx < dh->string_ids_size; ++sidx) {
    auto str = m_idx->get_stringidx(sidx);
    stats->strings_total_size += str->get_entry_size();
  }

  const dex_map_list* map_list =
      reinterpret_cast<const dex_map_list*>(m_file->const_data() + dh->map_off);
  bool header_seen = false;
  uint32_t header_index = 0;
  for (uint32_t i = 0; i < map_list->size; i++) {
    const auto& item = map_list->items[i];

    const uint8_t* encdata = m_idx->get_uleb_data(item.offset);
    const uint8_t* initial_encdata = encdata;

    switch (item.type) {
    case TYPE_HEADER_ITEM:
      always_assert_log(
          !header_seen,
          "Expected header_item to be unique in the map_list, "
          "but encountered one at index i=%u and another at index j=%u.",
          header_index,
          i);
      header_seen = true;
      header_index = i;
      always_assert_log(1 == item.size,
                        "Expected count of header_items in the map_list to be "
                        "exactly 1, but got ct=%u.",
                        item.size);
      stats->header_item_count += item.size;
      stats->header_item_bytes += item.size * sizeof(dex_header);
      break;
    case TYPE_STRING_ID_ITEM:
      stats->string_id_count += item.size;
      stats->string_id_bytes += item.size * sizeof(dex_string_id);
      break;
    case TYPE_TYPE_ID_ITEM:
      stats->type_id_count += item.size;
      stats->type_id_bytes += item.size * sizeof(dex_type_id);
      break;
    case TYPE_PROTO_ID_ITEM:
      stats->proto_id_count += item.size;
      stats->proto_id_bytes += item.size * sizeof(dex_proto_id);
      break;
    case TYPE_FIELD_ID_ITEM:
      stats->field_id_count += item.size;
      stats->field_id_bytes += item.size * sizeof(dex_field_id);
      break;
    case TYPE_METHOD_ID_ITEM:
      stats->method_id_count += item.size;
      stats->method_id_bytes += item.size * sizeof(dex_method_id);
      break;
    case TYPE_CLASS_DEF_ITEM:
      stats->class_def_count += item.size;
      stats->class_def_bytes += item.size * sizeof(dex_class_def);
      break;
    case TYPE_CALL_SITE_ID_ITEM:
      stats->call_site_id_count += item.size;
      stats->call_site_id_bytes += item.size * sizeof(dex_callsite_id);
      break;
    case TYPE_METHOD_HANDLE_ITEM:
      stats->method_handle_count += item.size;
      stats->method_handle_bytes += item.size * sizeof(dex_methodhandle_id);
      break;
    case TYPE_MAP_LIST:
      stats->map_list_count += item.size;
      for (uint32_t j = 0; j < item.size; j++) {
        encdata = align_ptr(encdata, 4);

        uint32_t map_list_entries = *(uint32_t*)(encdata);
        stats->map_list_bytes +=
            sizeof(uint32_t) + map_list_entries * sizeof(dex_map_item);
      }
      break;
    case TYPE_TYPE_LIST:
      stats->type_list_count += item.size;
      for (uint32_t j = 0; j < item.size; j++) {
        encdata = align_ptr(encdata, 4);

        uint32_t type_list_entries = *(uint32_t*)(encdata);
        stats->type_list_bytes +=
            sizeof(uint32_t) + type_list_entries * sizeof(dex_type_item);
      }
      break;
    case TYPE_ANNOTATION_SET_REF_LIST:
      stats->annotation_set_ref_list_count += item.size;
      for (uint32_t j = 0; j < item.size; j++) {
        encdata = align_ptr(encdata, 4);

        uint32_t annotation_set_ref_list_entries = *(uint32_t*)(encdata);
        stats->annotation_set_ref_list_bytes +=
            sizeof(uint32_t) + annotation_set_ref_list_entries *
                                   sizeof(dex_annotation_set_ref_item);
      }
      break;
    case TYPE_ANNOTATION_SET_ITEM:
      stats->annotation_set_count += item.size;
      for (uint32_t j = 0; j < item.size; j++) {
        encdata = align_ptr(encdata, 4);

        uint32_t annotation_set_entries = *(uint32_t*)(encdata);
        stats->annotation_set_bytes +=
            sizeof(uint32_t) +
            annotation_set_entries * sizeof(dex_annotation_off_item);
      }
      break;
    case TYPE_CLASS_DATA_ITEM:
      stats->class_data_count += item.size;

      for (uint32_t j = 0; j < item.size; j++) {
        // Read in field sizes.
        uint32_t static_fields_size = read_uleb128(&encdata);
        uint32_t instance_fields_size = read_uleb128(&encdata);
        uint32_t direct_methods_size = read_uleb128(&encdata);
        uint32_t virtual_methods_size = read_uleb128(&encdata);

        for (uint32_t k = 0; k < static_fields_size + instance_fields_size;
             ++k) {
          // Read and skip all of the encoded_field data.
          read_uleb128(&encdata);
          read_uleb128(&encdata);
        }

        for (uint32_t k = 0; k < direct_methods_size + virtual_methods_size;
             ++k) {
          // Read and skip all of the encoded_method data.
          read_uleb128(&encdata);
          read_uleb128(&encdata);
          read_uleb128(&encdata);
        }
      }

      stats->class_data_bytes += encdata - initial_encdata;
      break;
    case TYPE_CODE_ITEM:
      stats->code_count += item.size;

      for (uint32_t j = 0; j < item.size; j++) {
        encdata = align_ptr(encdata, 4);

        dex_code_item* code_item = (dex_code_item*)encdata;

        encdata += sizeof(dex_code_item);
        encdata += code_item->insns_size * sizeof(uint16_t);

        if (code_item->tries_size != 0 && code_item->insns_size % 2 == 1) {
          encdata += sizeof(uint16_t);
        }

        encdata += code_item->tries_size * sizeof(dex_tries_item);

        if (code_item->tries_size != 0) {
          uint32_t catch_handler_list_size = read_uleb128(&encdata);
          for (uint32_t k = 0; k < catch_handler_list_size; ++k) {
            int32_t catch_handler_size = read_sleb128(&encdata);
            uint32_t abs_size = (uint32_t)std::abs(catch_handler_size);
            for (uint32_t l = 0; l < abs_size; ++l) {
              // Read encoded_type_addr_pair.
              read_uleb128(&encdata);
              read_uleb128(&encdata);
            }
            // Read catch_all_addr
            if (catch_handler_size <= 0) {
              read_uleb128(&encdata);
            }
          }
        }
      }
      stats->code_bytes += encdata - initial_encdata;
      break;
    case TYPE_STRING_DATA_ITEM:
      stats->string_data_count += item.size;

      for (uint32_t j = 0; j < item.size; j++) {
        // Skip data that encodes the number of UTF-16 code units.
        read_uleb128(&encdata);

        // Read up to and including the NULL-terminating byte.
        while (true) {
          const uint8_t byte = *encdata;
          encdata++;
          if (byte == 0) break;
        }
      }

      stats->string_data_bytes += encdata - initial_encdata;
      break;
    case TYPE_DEBUG_INFO_ITEM:
      stats->num_dbg_items += item.size;
      for (uint32_t j = 0; j < item.size; j++) {
        // line_start
        read_uleb128(&encdata);
        // param_count
        uint32_t param_count = read_uleb128(&encdata);
        while (param_count--) {
          // Each parameter is one uleb128p1
          read_uleb128p1(&encdata);
        }
        bool running = true;
        while (running) {
          uint8_t opcode = *encdata++;
          switch (opcode) {
          case DBG_END_SEQUENCE:
            running = false;
            break;
          case DBG_ADVANCE_PC:
          case DBG_END_LOCAL:
          case DBG_RESTART_LOCAL:
            // each of these opcodes has one uleb128 arg:
            // - addr_diff
            // - register_num
            // - register_num
            read_uleb128(&encdata);
            break;
          case DBG_ADVANCE_LINE:
            // line_diff
            read_sleb128(&encdata);
            break;
          case DBG_START_LOCAL:
            // register_num
            read_uleb128(&encdata);
            // name_idx
            read_uleb128p1(&encdata);
            // type_idx
            read_uleb128p1(&encdata);
            break;
          case DBG_START_LOCAL_EXTENDED:
            // register_num
            read_uleb128(&encdata);
            // name_idx
            read_uleb128p1(&encdata);
            // type_idx
            read_uleb128p1(&encdata);
            // sig_idx
            read_uleb128p1(&encdata);
            break;
          case DBG_SET_FILE:
            // name_idx
            read_uleb128p1(&encdata);
            break;
          case DBG_SET_PROLOGUE_END:
          case DBG_SET_EPILOGUE_BEGIN:
            // These cases have no args
            break;
          default:
            // These are special opcodes. We separate them out to the default
            // case to show we're properly interpretting this program.
            break;
          }
        }
      }
      stats->dbg_total_size += encdata - initial_encdata;
      break;
    case TYPE_ANNOTATION_ITEM:
      // TBD!
      break;
    case TYPE_ENCODED_ARRAY_ITEM:
      // TBD!
      break;
    case TYPE_ANNOTATIONS_DIR_ITEM:
      stats->annotations_directory_count += item.size;

      for (uint32_t j = 0; j < item.size; ++j) {
        encdata = align_ptr(encdata, 4);
        dex_annotations_directory_item* annotations_directory_item =
            (dex_annotations_directory_item*)encdata;

        encdata += sizeof(dex_annotations_directory_item);
        encdata += sizeof(dex_field_annotation) *
                   annotations_directory_item->fields_size;
        encdata += sizeof(dex_method_annotation) *
                   annotations_directory_item->methods_size;
        encdata += sizeof(dex_parameter_annotation) *
                   annotations_directory_item->parameters_size;
      }

      stats->annotations_directory_bytes += encdata - initial_encdata;
      break;
    case TYPE_HIDDENAPI_CLASS_DATA_ITEM:
      // No stats gathered.
      break;
    default:
      fprintf(
          stderr,
          "warning: map_list item at index i=%u is of unknown type T=0x%04hX\n",
          i,
          item.type);
    }
  }
}

void DexLoader::load_dex_class(int num) {
  const dex_class_def* cdef = m_class_defs + num;
  DexClass* dc = DexClass::create(m_idx.get(), cdef, m_location);
  // We may be inserting a nullptr here. Need to remove them later
  //
  // We're inserting nullptr because we can't mess up the indices of the other
  // classes in the vector. This vector is used via random access.
  m_classes->at(num) = dc;
}

const dex_header* DexLoader::get_dex_header(const char* file_name) {
  m_file->open(file_name, boost::iostreams::mapped_file::readonly);
  if (!m_file->is_open()) {
    fprintf(stderr, "error: cannot create memory-mapped file: %s\n", file_name);
    exit(EXIT_FAILURE);
  }
  return reinterpret_cast<const dex_header*>(m_file->const_data());
}

DexClasses DexLoader::load_dex(const char* file_name,
                               dex_stats_t* stats,
                               int support_dex_version) {
  const dex_header* dh = get_dex_header(file_name);
  validate_dex_header(dh, m_file->size(), support_dex_version);
  return load_dex(dh, stats);
}

DexClasses DexLoader::load_dex(const dex_header* dh, dex_stats_t* stats) {
  if (dh->class_defs_size == 0) {
    return DexClasses(0);
  }
  m_idx = std::make_unique<DexIdx>(dh);
  auto off = (uint64_t)dh->class_defs_off;
  m_class_defs =
      reinterpret_cast<const dex_class_def*>((const uint8_t*)dh + off);
  DexClasses classes(dh->class_defs_size);
  m_classes = &classes;

  {
    auto num_threads = redex_parallel::default_num_threads();
    std::vector<std::exception_ptr> all_exceptions;
    std::mutex all_exceptions_mutex;
    workqueue_run_for<size_t>(
        0, dh->class_defs_size,
        [&all_exceptions, &all_exceptions_mutex, this](uint32_t num) {
          try {
            load_dex_class(num);
          } catch (const std::exception& exc) {
            TRACE(MAIN, 1, "Worker throw the exception:%s", exc.what());
            std::lock_guard<std::mutex> lock_guard(all_exceptions_mutex);
            all_exceptions.emplace_back(std::current_exception());
          }
        },
        num_threads);

    if (!all_exceptions.empty()) {
      // At least one of the workers raised an exception
      aggregate_exception ae(all_exceptions);
      throw ae;
    }
  }

  gather_input_stats(stats, dh);

  // Remove nulls from the classes list. They may have been introduced by benign
  // duplicate classes.
  classes.erase(std::remove(classes.begin(), classes.end(), nullptr),
                classes.end());

  return classes;
}

static void balloon_all(const Scope& scope, bool throw_on_error) {
  ConcurrentMap<DexMethod*, std::string> ir_balloon_errors;
  walk::parallel::methods(scope, [&](DexMethod* m) {
    if (m->get_dex_code()) {
      try {
        m->balloon();
      } catch (RedexException& re) {
        ir_balloon_errors.emplace(m, re.what());
      }
    }
  });

  if (!ir_balloon_errors.empty()) {
    std::ostringstream oss;
    oss << "Error lifting DexCode to IRCode for the following methods:"
        << std::endl;
    for (const auto& [method, msg] : ir_balloon_errors) {
      oss << show(method) << ": " << msg << std::endl;
    }

    always_assert_log(!throw_on_error,
                      "%s" /* format string must be a string literal */,
                      oss.str().c_str());
    TRACE(MAIN, 1, "%s" /* format string must be a string literal */,
          oss.str().c_str());
  }
}

DexClasses load_classes_from_dex(const DexLocation* location,
                                 bool balloon,
                                 bool throw_on_balloon_error,
                                 int support_dex_version) {
  dex_stats_t stats;
  return load_classes_from_dex(location, &stats, balloon,
                               throw_on_balloon_error, support_dex_version);
}

DexClasses load_classes_from_dex(const DexLocation* location,
                                 dex_stats_t* stats,
                                 bool balloon,
                                 bool throw_on_balloon_error,
                                 int support_dex_version) {
  TRACE(MAIN, 1, "Loading classes from dex from %s",
        location->get_file_name().c_str());
  DexLoader dl(location);
  auto classes = dl.load_dex(location->get_file_name().c_str(), stats,
                             support_dex_version);
  if (balloon) {
    balloon_all(classes, throw_on_balloon_error);
  }
  return classes;
}

DexClasses load_classes_from_dex(const dex_header* dh,
                                 const DexLocation* location,
                                 bool balloon,
                                 bool throw_on_balloon_error) {
  DexLoader dl(location);
  auto classes = dl.load_dex(dh, nullptr);
  if (balloon) {
    balloon_all(classes, throw_on_balloon_error);
  }
  return classes;
}

std::string load_dex_magic_from_dex(const DexLocation* location) {
  DexLoader dl(location);
  auto dh = dl.get_dex_header(location->get_file_name().c_str());
  return dh->magic;
}

void balloon_for_test(const Scope& scope) { balloon_all(scope, true); }
