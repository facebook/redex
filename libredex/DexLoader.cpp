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

namespace {

// Lazy eval system. Because of the map interface this is a bit
// annoying to implement nicely. Current design uses RAII

struct AssertHelper {
  std::optional<std::string> message{};
  std::optional<std::map<std::string, std::string>> extra_info{};

  explicit AssertHelper() {}

  // Yeah, it's not good form to throw in a destructor. But...
  // NOLINTNEXTLINE(bugprone-exception-escape)
  ~AssertHelper() noexcept(false) {
    assert_or_throw(
        false, RedexError::INVALID_DEX, message ? *message : std::string(""),
        extra_info ? *extra_info : std::map<std::string, std::string>());
  }

  AssertHelper& set_message(std::string&& msg) {
    message = std::move(msg);
    return *this;
  }

  template <typename T>
  AssertHelper& add_info(std::string&& key, T value) {
    if (!extra_info) {
      extra_info = std::map<std::string, std::string>();
    }
    extra_info->emplace(key, std::to_string(value));
    return *this;
  }
};

// std::to_string doesn't take std::string, so need to specialize.
template <>
AssertHelper& AssertHelper::add_info<std::string>(std::string&& key,
                                                  std::string value) {
  if (!extra_info) {
    extra_info = std::map<std::string, std::string>();
  }
  extra_info->emplace(key, std::move(value));
  return *this;
}

// Macro for lazy eval. The exception interface is not great.
//
// This leaves an "open" AssertHelper that can be filled with
// literate style calls:
// DEX_ASSERT(x == y).set_message("x not equal y").add_info("x",
// x).add_info("z", z);
#define DEX_ASSERT(expr) \
  if (!(expr)) AssertHelper()

void dex_range_assert(uint64_t offset,
                      size_t extent,
                      size_t dexsize,
                      const std::string& msg_invalid,
                      const std::string& msg_invalid_extent,
                      const std::string& offset_name) {
  assert_or_throw(offset < dexsize, RedexError::INVALID_DEX, msg_invalid,
                  {{offset_name, std::to_string(offset)},
                   {"extent", std::to_string(extent)},
                   {"dex_size", std::to_string(dexsize)}});

  assert_or_throw(extent <= dexsize && offset <= dexsize - extent,
                  RedexError::INVALID_DEX,
                  msg_invalid_extent,
                  {{offset_name, std::to_string(offset)},
                   {"extent", std::to_string(extent)},
                   {"dex_size", std::to_string(dexsize)}});
}

template <typename T>
void dex_type_range_assert(uint64_t offset,
                           uint64_t size,
                           size_t dexsize,
                           const std::string& type_name) {
  // The string operations are a bit annoying as they are not lazy.
  dex_range_assert(offset,
                   size * sizeof(T),
                   dexsize,
                   type_name + " out of range",
                   "invalid " + type_name + " size",
                   type_name + "_off");
}

uint32_t read_uleb128_checked(std::string_view& ptr) {
  auto next = [&ptr]() {
    always_assert_type_log(!ptr.empty(), INVALID_DEX, "ULEB128 underflow");
    uint8_t result = *ptr.data();
    ptr = ptr.substr(1);
    return result;
  };

  uint32_t result = next();
  if (result > 0x7f) {
    uint32_t cur = next();
    result = (result & 0x7f) | ((cur & 0x7f) << 7);
    if (cur > 0x7f) {
      cur = next();
      result |= (cur & 0x7f) << 14;
      if (cur > 0x7f) {
        cur = next();
        result |= (cur & 0x7f) << 21;
        if (cur > 0x7f) {
          cur = next();
          result |= cur << 28;
        }
      }
    }
  }
  return result;
}

int32_t read_sleb128_checked(std::string_view& ptr) {
  auto next = [&ptr]() {
    always_assert_type_log(!ptr.empty(), INVALID_DEX, "SLEB128 underflow");
    uint8_t result = *ptr.data();
    ptr = ptr.substr(1);
    return result;
  };

  int32_t result = next();

  if (result <= 0x7f) {
    result = (result << 25) >> 25;
  } else {
    int cur = next();
    result = (result & 0x7f) | ((cur & 0x7f) << 7);
    if (cur <= 0x7f) {
      result = (result << 18) >> 18;
    } else {
      cur = next();
      result |= (cur & 0x7f) << 14;
      if (cur <= 0x7f) {
        result = (result << 11) >> 11;
      } else {
        cur = next();
        result |= (cur & 0x7f) << 21;
        if (cur <= 0x7f) {
          result = (result << 4) >> 4;
        } else {
          cur = next();
          // Change from AOSP: avoid undefined shifting behavior.
          result |= (cur & 0x0f) << 28;
        }
      }
    }
  }
  return result;
}

void align_ptr(std::string_view& ptr, const size_t alignment) {
  const size_t alignment_error = ((uintptr_t)ptr.data()) % alignment;
  if (alignment_error != 0) {
    ptr = ptr.substr(alignment - alignment_error);
  }
}

} // namespace

static void validate_dex_header(const dex_header* dh,
                                size_t dexsize,
                                int support_dex_version) {
  DEX_ASSERT(sizeof(dex_header) <= dexsize)
      .set_message("Header size is larger than file size")
      .add_info("header_size", sizeof(dex_header))
      .add_info("dex_size", dexsize);

  // Cleanliness check. Also helps with fuzzers creating at least halfway
  // valid files that may be dumped.
  DEX_ASSERT(dh->endian_tag == ENDIAN_CONSTANT)
      .set_message("Bad/unsupported endian tag")
      .add_info("tag", dh->endian_tag);

  bool supported = false;
  switch (support_dex_version) {
  case 39:
    supported = supported ||
                !memcmp(dh->magic, DEX_HEADER_DEXMAGIC_V39, sizeof(dh->magic));
    FALLTHROUGH_INTENDED; /* intentional fallthrough to also check for v38 */
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
  DEX_ASSERT(supported)
      .set_message("Bad dex magic for support_dex_version")
      .add_info("magic", std::string(dh->magic, sizeof(dh->magic)))
      .add_info("support_dex_version", support_dex_version);
  DEX_ASSERT(dh->file_size == dexsize)
      .set_message("Reported size in header does not match file size")
      .add_info("dexsize", dexsize)
      .add_info("header_size", dh->file_size);

  auto map_list = [&]() {
    auto map_list_off = (uint64_t)dh->map_off;
    dex_range_assert(map_list_off, sizeof(dex_map_list), dexsize,
                     "map_off invalid", "map_list out of range (struct)",
                     "map_list_off");
    const dex_map_list* map_list_tmp =
        (dex_map_list*)(((const uint8_t*)dh) + dh->map_off);
    dex_range_assert(
        map_list_off,
        sizeof(uint32_t) + map_list_tmp->size * sizeof(dex_map_item), dexsize,
        "map_off invalid", "map_list out of range (data)", "map_list_off");
    return map_list_tmp;
  }();

  for (uint32_t i = 0; i < map_list->size; i++) {
    auto& item = map_list->items[i];
    switch (item.type) {
    case TYPE_CALL_SITE_ID_ITEM: {
      auto callsite_ids_off = (uint64_t)item.offset;
      dex_type_range_assert<dex_callsite_id>(callsite_ids_off, item.size,
                                             dexsize, "callsite_ids");
      // dex_callsite_id* callsite_ids =
      //     (dex_callsite_id*)((uint8_t*)dh + item.offset);
    } break;
    case TYPE_METHOD_HANDLE_ITEM: {
      auto methodhandle_ids_off = (uint64_t)item.offset;
      dex_type_range_assert<dex_methodhandle_id>(
          methodhandle_ids_off, item.size, dexsize, "methodhandle_ids");
      // dex_methodhandle_id* methodhandle_ids =
      //     (dex_methodhandle_id*)((uint8_t*)dh + item.offset);
    } break;
    }
  }

  dex_type_range_assert<dex_string_id>(dh->string_ids_off, dh->string_ids_size,
                                       dexsize, "string_ids");

  dex_type_range_assert<dex_type_id>(dh->type_ids_off, dh->type_ids_size,
                                     dexsize, "type_ids");

  dex_type_range_assert<dex_proto_id>(dh->proto_ids_off, dh->proto_ids_size,
                                      dexsize, "proto_ids");

  dex_type_range_assert<dex_field_id>(dh->field_ids_off, dh->field_ids_size,
                                      dexsize, "field_ids");

  dex_type_range_assert<dex_method_id>(dh->method_ids_off, dh->method_ids_size,
                                       dexsize, "method_ids");

  dex_type_range_assert<dex_class_def>(dh->class_defs_off, dh->class_defs_size,
                                       dexsize, "class_defs");
}

namespace {

template <typename T>
const T* get_and_consume(std::string_view& ptr, size_t align) {
  if (align > 1) {
    align_ptr(ptr, align);
  }
  always_assert_type_log(ptr.size() >= sizeof(T), INVALID_DEX,
                         "Dex out of bounds");
  const T* result = (const T*)ptr.data();
  ptr = ptr.substr(sizeof(T));
  return result;
}

} // namespace

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
          m_idx->get_data<dex_annotations_directory_item>(anno_off);
      auto class_anno_off = anno_dir->class_annotations_off;
      if (class_anno_off) {
        const uint32_t* anno_data = m_idx->get_uint_data(class_anno_off);
        uint32_t count = *anno_data++;
        always_assert(anno_data <= anno_data + count);
        always_assert((uint8_t*)(anno_data + count) <= m_idx->end());
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
          always_assert(annoxref < annoxref + count);
          always_assert((uint8_t*)(annoxref + count) <= m_idx->end());
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
    auto process_methods = [&](const auto& methods) {
      for (auto* meth : methods) {
        DexCode* code = meth->get_dex_code();
        if (code) {
          stats->num_instructions += code->get_instructions().size();
          stats->num_tries += code->get_tries().size();
        }
      }
    };
    process_methods(clz->get_vmethods());
    process_methods(clz->get_dmethods());
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

    std::string_view encdata{(const char*)m_idx->get_uleb_data(item.offset),
                             m_idx->get_file_size() - item.offset};

    auto consume_encdata = [&](size_t size) {
      always_assert_type_log(encdata.size() >= size, INVALID_DEX,
                             "Dex out of bounds");
      encdata = encdata.substr(size);
    };

    switch (item.type) {
    case TYPE_HEADER_ITEM:
      DEX_ASSERT(!header_seen)
          .set_message("Expected header_item to be unique in the map_list")
          .add_info("i", header_index)
          .add_info("j", i);
      header_seen = true;
      header_index = i;
      DEX_ASSERT(1 == item.size)
          .set_message(
              "Expected count of header_items in the map_list to be exactly 1")
          .add_info("size", item.size);
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
        uint32_t map_list_entries = *get_and_consume<uint32_t>(encdata, 4);
        stats->map_list_bytes +=
            sizeof(uint32_t) + map_list_entries * sizeof(dex_map_item);
        consume_encdata(map_list_entries * sizeof(dex_map_item));
      }
      break;
    case TYPE_TYPE_LIST:
      stats->type_list_count += item.size;
      for (uint32_t j = 0; j < item.size; j++) {
        uint32_t type_list_entries = *get_and_consume<uint32_t>(encdata, 4);
        stats->type_list_bytes +=
            sizeof(uint32_t) + type_list_entries * sizeof(dex_type_item);
        consume_encdata(type_list_entries * sizeof(dex_type_item));
      }
      break;
    case TYPE_ANNOTATION_SET_REF_LIST:
      stats->annotation_set_ref_list_count += item.size;
      for (uint32_t j = 0; j < item.size; j++) {
        uint32_t annotation_set_ref_list_entries =
            *get_and_consume<uint32_t>(encdata, 4);
        stats->annotation_set_ref_list_bytes +=
            sizeof(uint32_t) + annotation_set_ref_list_entries *
                                   sizeof(dex_annotation_set_ref_item);
        consume_encdata(annotation_set_ref_list_entries *
                        sizeof(dex_annotation_set_ref_item));
      }
      break;
    case TYPE_ANNOTATION_SET_ITEM:
      stats->annotation_set_count += item.size;
      for (uint32_t j = 0; j < item.size; j++) {
        uint32_t annotation_set_entries =
            *get_and_consume<uint32_t>(encdata, 4);
        stats->annotation_set_bytes +=
            sizeof(uint32_t) +
            annotation_set_entries * sizeof(dex_annotation_off_item);
        consume_encdata(annotation_set_entries *
                        sizeof(dex_annotation_off_item));
      }
      break;
    case TYPE_CLASS_DATA_ITEM: {
      size_t orig_size = encdata.size();

      stats->class_data_count += item.size;

      for (uint32_t j = 0; j < item.size; j++) {
        // Read in field sizes.
        uint32_t static_fields_size = read_uleb128_checked(encdata);
        uint32_t instance_fields_size = read_uleb128_checked(encdata);
        uint32_t direct_methods_size = read_uleb128_checked(encdata);
        uint32_t virtual_methods_size = read_uleb128_checked(encdata);

        for (uint32_t k = 0; k < static_fields_size + instance_fields_size;
             ++k) {
          // Read and skip all of the encoded_field data.
          read_uleb128_checked(encdata);
          read_uleb128_checked(encdata);
        }

        for (uint32_t k = 0; k < direct_methods_size + virtual_methods_size;
             ++k) {
          // Read and skip all of the encoded_method data.
          read_uleb128_checked(encdata);
          read_uleb128_checked(encdata);
          read_uleb128_checked(encdata);
        }
      }

      stats->class_data_bytes += orig_size - encdata.size();
      break;
    }
    case TYPE_CODE_ITEM: {
      size_t orig_size = encdata.size();

      stats->code_count += item.size;

      for (uint32_t j = 0; j < item.size; j++) {
        auto* code_item = get_and_consume<dex_code_item>(encdata, 4);

        consume_encdata(code_item->insns_size * sizeof(uint16_t));

        if (code_item->tries_size != 0 && code_item->insns_size % 2 == 1) {
          consume_encdata(sizeof(uint16_t));
        }

        consume_encdata(code_item->tries_size * sizeof(dex_tries_item));

        if (code_item->tries_size != 0) {
          uint32_t catch_handler_list_size = read_uleb128_checked(encdata);
          for (uint32_t k = 0; k < catch_handler_list_size; ++k) {
            int32_t catch_handler_size = read_sleb128_checked(encdata);
            uint32_t abs_size = (uint32_t)std::abs(catch_handler_size);
            for (uint32_t l = 0; l < abs_size; ++l) {
              // Read encoded_type_addr_pair.
              read_uleb128_checked(encdata);
              read_uleb128_checked(encdata);
            }
            // Read catch_all_addr
            if (catch_handler_size <= 0) {
              read_uleb128_checked(encdata);
            }
          }
        }
      }
      stats->code_bytes += orig_size - encdata.size();
      break;
    }
    case TYPE_STRING_DATA_ITEM: {
      size_t orig_size = encdata.size();

      stats->string_data_count += item.size;

      for (uint32_t j = 0; j < item.size; j++) {
        // Skip data that encodes the number of UTF-16 code units.
        read_uleb128_checked(encdata);

        // Read up to and including the NULL-terminating byte.
        while (*get_and_consume<char>(encdata, 1) != '\0') {
        }
      }

      stats->string_data_bytes += encdata.size() - orig_size;
      break;
    }
    case TYPE_DEBUG_INFO_ITEM: {
      size_t orig_size = encdata.size();

      stats->num_dbg_items += item.size;
      for (uint32_t j = 0; j < item.size; j++) {
        // line_start
        read_uleb128_checked(encdata);
        // param_count
        uint32_t param_count = read_uleb128_checked(encdata);
        while (param_count--) {
          // Each parameter is one uleb128p1
          read_uleb128_checked(encdata);
        }
        bool running = true;
        while (running) {
          uint8_t opcode = *get_and_consume<uint8_t>(encdata, 1);
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
            read_uleb128_checked(encdata);
            break;
          case DBG_ADVANCE_LINE:
            // line_diff
            read_sleb128_checked(encdata);
            break;
          case DBG_START_LOCAL:
            // register_num
            read_uleb128_checked(encdata);
            // name_idx
            read_uleb128_checked(encdata);
            // type_idx
            read_uleb128_checked(encdata);
            break;
          case DBG_START_LOCAL_EXTENDED:
            // register_num
            read_uleb128_checked(encdata);
            // name_idx
            read_uleb128_checked(encdata);
            // type_idx
            read_uleb128_checked(encdata);
            // sig_idx
            read_uleb128_checked(encdata);
            break;
          case DBG_SET_FILE:
            // name_idx
            read_uleb128_checked(encdata);
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
      stats->dbg_total_size += orig_size - encdata.size();
      break;
    }
    case TYPE_ANNOTATION_ITEM:
      // TBD!
      break;
    case TYPE_ENCODED_ARRAY_ITEM:
      // TBD!
      break;
    case TYPE_ANNOTATIONS_DIR_ITEM: {
      size_t orig_size = encdata.size();

      stats->annotations_directory_count += item.size;

      for (uint32_t j = 0; j < item.size; ++j) {
        auto* annotations_directory_item =
            get_and_consume<dex_annotations_directory_item>(encdata, 4);

        size_t advance = sizeof(dex_field_annotation) *
                             annotations_directory_item->fields_size +
                         sizeof(dex_method_annotation) *
                             annotations_directory_item->methods_size +
                         sizeof(dex_parameter_annotation) *
                             annotations_directory_item->parameters_size;
        consume_encdata(advance);
      }

      stats->annotations_directory_bytes += encdata.size() - orig_size;
      break;
    }
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
  size_t dexsize = m_file->size();
  const dex_class_def* cdef = m_class_defs + num;
  auto idx = m_idx.get();

  // Validate dex_class_def layout
  auto annotations_off = cdef->annotations_off;
  if (annotations_off != 0) {
    // Validate dex_annotations_directory_item layout
    dex_type_range_assert<dex_annotations_directory_item>(
        annotations_off, 1, dexsize, "cdef->annotations");
    const dex_annotations_directory_item* annodir =
        idx->get_data<dex_annotations_directory_item>(annotations_off);
    auto cls_annos_off = annodir->class_annotations_off;
    DEX_ASSERT(cls_annos_off < dexsize)
        .set_message("Invalid annodir->class_annotations_off");
    if (cls_annos_off != 0) {
      // annotation_off_item is of size uint. So this is probably precise
      // enough.
      const uint32_t* adata = idx->get_uint_data(cls_annos_off);
      uint32_t count = *adata;
      DEX_ASSERT(cls_annos_off + count <= dexsize)
          .set_message("Invalid class annotation set count")
          .add_info("cls_annos_off", cls_annos_off)
          .add_info("count", count)
          .add_info("dexsize", dexsize);
    }
  }

  DexClass* dc = DexClass::create(idx, cdef, m_location);
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
                               int support_dex_version,
                               Parallel p) {
  const dex_header* dh = get_dex_header(file_name);
  validate_dex_header(dh, m_file->size(), support_dex_version);
  return load_dex(dh, stats, p);
}

DexClasses DexLoader::load_dex(const dex_header* dh,
                               dex_stats_t* stats,
                               Parallel p) {
  if (dh->class_defs_size == 0) {
    return DexClasses(0);
  }
  m_idx = std::make_unique<DexIdx>(dh);
  auto off = (uint64_t)dh->class_defs_off;
  m_class_defs =
      reinterpret_cast<const dex_class_def*>((const uint8_t*)dh + off);
  DexClasses classes(dh->class_defs_size);
  m_classes = &classes;

  switch (p) {
  case Parallel::kNo: {
    for (size_t i = 0; i < dh->class_defs_size; ++i) {
      load_dex_class(i);
    }
    break;
  }
  case Parallel::kYes: {
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
      aggregate_exception ae(all_exceptions);
      throw ae;
    }
    break;
  }
  }

  gather_input_stats(stats, dh);

  // Remove nulls from the classes list. They may have been introduced by benign
  // duplicate classes.
  classes.erase(std::remove(classes.begin(), classes.end(), nullptr),
                classes.end());

  return classes;
}

static void balloon_all(const Scope& scope,
                        bool throw_on_error,
                        DexLoader::Parallel p) {
  switch (p) {
  case DexLoader::Parallel::kNo: {
    walk::methods(scope, [&](DexMethod* m) {
      if (m->get_dex_code()) {
        m->balloon();
      }
    });
    break;
  }
  case DexLoader::Parallel::kYes: {
    InsertOnlyConcurrentMap<DexMethod*,
                            std::pair<std::string, std::exception_ptr>>
        ir_balloon_errors;
    walk::parallel::methods(scope, [&](DexMethod* m) {
      if (m->get_dex_code()) {
        try {
          m->balloon();
        } catch (RedexException& re) {
          ir_balloon_errors.emplace(
              m, std::make_pair(re.what(), std::make_exception_ptr(re)));
        }
      }
    });

    if (!ir_balloon_errors.empty()) {
      if (throw_on_error) {
        std::vector<std::exception_ptr> all_exceptions;
        for (const auto& [_, data] : ir_balloon_errors) {
          all_exceptions.emplace_back(data.second);
        }
        throw aggregate_exception(std::move(all_exceptions));
      }

      std::ostringstream oss;
      oss << "Error lifting DexCode to IRCode for the following methods:"
          << std::endl;
      for (const auto& [method, data] : ir_balloon_errors) {
        oss << show(method) << ": " << data.first << std::endl;
      }

      TRACE(MAIN, 1, "%s" /* format string must be a string literal */,
            oss.str().c_str());
    }
    break;
  }
  }
}

DexClasses load_classes_from_dex(const DexLocation* location,
                                 bool balloon,
                                 bool throw_on_balloon_error,
                                 int support_dex_version,
                                 DexLoader::Parallel p) {
  dex_stats_t stats;
  return load_classes_from_dex(location, &stats, balloon,
                               throw_on_balloon_error, support_dex_version, p);
}

DexClasses load_classes_from_dex(const DexLocation* location,
                                 dex_stats_t* stats,
                                 bool balloon,
                                 bool throw_on_balloon_error,
                                 int support_dex_version,
                                 DexLoader::Parallel p) {
  TRACE(MAIN, 1, "Loading classes from dex from %s",
        location->get_file_name().c_str());
  DexLoader dl(location);
  auto classes = dl.load_dex(location->get_file_name().c_str(), stats,
                             support_dex_version, p);
  if (balloon) {
    balloon_all(classes, throw_on_balloon_error, p);
  }
  return classes;
}

DexClasses load_classes_from_dex(const dex_header* dh,
                                 const DexLocation* location,
                                 bool balloon,
                                 bool throw_on_balloon_error,
                                 DexLoader::Parallel p) {
  DexLoader dl(location);
  auto classes = dl.load_dex(dh, nullptr, p);
  if (balloon) {
    balloon_all(classes, throw_on_balloon_error, p);
  }
  return classes;
}

std::string load_dex_magic_from_dex(const DexLocation* location) {
  DexLoader dl(location);
  auto dh = dl.get_dex_header(location->get_file_name().c_str());
  return dh->magic;
}

void balloon_for_test(const Scope& scope) {
  balloon_all(scope, true, DexLoader::Parallel::kYes);
}
