/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "utils/Visitor.h"
#include "androidfw/TypeWrappers.h"

#include <limits>
#include <sstream>
#include <string>

namespace arsc {

#define VERY_VERBOSE false

#if VERY_VERBOSE
#define LOGVV(...) ALOGV(__VA_ARGS__)
#else
#define LOGVV(...) \
  do {             \
  } while (0)
#endif

// Resource table structs have been added to over time. Define a set of
// backwards compatible minimum known sizes for the structs that could exist
// if generated via old tools.
constexpr size_t MIN_PACKAGE_SIZE =
    sizeof(android::ResTable_package) -
    sizeof(android::ResTable_package::typeIdOffset);
// The minimum size required to read any version of ResTable_type. ResTable_type
// has a ResTable_config, and ResTable_config has been augmented several times
// (and itself will denote its size) thus the smallest conceviable config is
// just a 4 byte int denoting that.
constexpr size_t MIN_RES_TABLE_TYPE_SIZE =
    sizeof(android::ResTable_type) - sizeof(android::ResTable_config) +
    sizeof(android::ResTable_config::size);

inline std::string dump_chunk(android::ResChunk_header* header) {
  std::stringstream ss;
  ss << "type=" << std::hex << dtohs(header->type);
  ss << " header_size=" << dtohs(header->headerSize);
  ss << " size=" << dtohl(header->size);
  return ss.str();
}

template <typename T, size_t MinSize = sizeof(T)>
inline static T* convert_chunk(android::ResChunk_header* chunk) {
  if (dtohs(chunk->headerSize) < MinSize) {
    return nullptr;
  }
  return reinterpret_cast<T*>(chunk);
}

inline static uint8_t* get_data(android::ResChunk_header* chunk) {
  return reinterpret_cast<uint8_t*>(chunk) + dtohs(chunk->headerSize);
}

inline static uint32_t get_data_len(android::ResChunk_header* chunk) {
  return dtohl(chunk->size) - dtohs(chunk->headerSize);
}

// Modeled after aapt2's ResChunkPullParser. Simple iteration over
// ResChunk_header structs with validation of sizes in the header.
class ResChunkPullParser {
 public:
  enum class Event {
    StartDocument,
    EndDocument,
    BadDocument,
    Chunk,
  };

  // Returns false if the event is EndDocument or BadDocument.
  static bool IsGoodEvent(Event event) {
    return event != Event::EndDocument && event != Event::BadDocument;
  }

  ResChunkPullParser(void* data, size_t len)
      : m_event(Event::StartDocument),
        m_data(reinterpret_cast<android::ResChunk_header*>(data)),
        m_len(len),
        m_current_chunk(nullptr) {}

  Event event() { return m_event; }

  android::ResChunk_header* chunk() { return m_current_chunk; }

  // Move to the next android::ResChunk_header.
  Event Next() {
    if (!IsGoodEvent(m_event)) {
      return m_event;
    }

    if (m_event == Event::StartDocument) {
      m_current_chunk = m_data;
    } else {
      m_current_chunk =
          (android::ResChunk_header*)(((const char*)m_current_chunk) +
                                      dtohl(m_current_chunk->size));
    }

    const std::ptrdiff_t diff =
        (const char*)m_current_chunk - (const char*)m_data;
    LOG_FATAL_IF(diff < 0, "diff is negative");
    const size_t offset = static_cast<const size_t>(diff);

    if (offset == m_len) {
      m_current_chunk = nullptr;
      return (m_event = Event::EndDocument);
    } else if (offset + sizeof(android::ResChunk_header) > m_len) {
      ALOGE("chunk is past the end of the document");
      m_current_chunk = nullptr;
      return (m_event = Event::BadDocument);
    }

    if (dtohs(m_current_chunk->headerSize) < sizeof(android::ResChunk_header)) {
      ALOGE("chunk has too small header");
      m_current_chunk = nullptr;
      return (m_event = Event::BadDocument);
    } else if (dtohl(m_current_chunk->size) <
               dtohs(m_current_chunk->headerSize)) {
      ALOGE("chunk's total size is smaller than header %s",
            dump_chunk(m_current_chunk).c_str());
      m_current_chunk = nullptr;
      return (m_event = Event::BadDocument);
    } else if (offset + dtohl(m_current_chunk->size) > m_len) {
      ALOGE("chunk's data extends past the end of the document %s",
            dump_chunk(m_current_chunk).c_str());
      m_current_chunk = nullptr;
      return (m_event = Event::BadDocument);
    }
    return (m_event = Event::Chunk);
  }

 private:
  Event m_event;
  android::ResChunk_header* m_data;
  size_t m_len;
  android::ResChunk_header* m_current_chunk;
};

void collect_spans(android::ResStringPool_span* ptr,
                   std::vector<android::ResStringPool_span*>* out) {
  while (dtohl(*reinterpret_cast<const uint32_t*>(ptr)) !=
         android::ResStringPool_span::END) {
    out->emplace_back(ptr);
    ptr++;
  }
}

bool ResourceTableVisitor::valid(const android::ResTable_package* package) {
  if (package == nullptr) {
    return false;
  }
  uint32_t package_id = dtohl(package->id);
  if (package_id > std::numeric_limits<uint8_t>::max()) {
    ALOGE("Package ID is too big: %x. Offset = %ld",
          package_id,
          get_file_offset(package));
    return false;
  }
  return true;
}

bool ResourceTableVisitor::valid(const android::ResTable_typeSpec* type_spec) {
  if (type_spec == nullptr) {
    return false;
  }
  if (type_spec->id == 0) {
    ALOGE("ResTable_typeSpec has invalid id: %x. Offset = %ld",
          type_spec->id,
          get_file_offset(type_spec));
    return false;
  }
  const size_t entry_count = dtohl(type_spec->entryCount);
  // Lower two bytes of a resource ID are used to denote entries.
  if (entry_count > std::numeric_limits<uint16_t>::max()) {
    ALOGE("ResTable_typeSpec has too many entries: %zu. Offset = %ld",
          entry_count,
          get_file_offset(type_spec));
    return false;
  }
  return true;
}

bool ResourceTableVisitor::valid(const android::ResTable_type* type) {
  if (type == nullptr) {
    return false;
  }
  if (type->id == 0) {
    ALOGE("ResTable_type has invalid id. Offset = %ld", get_file_offset(type));
    return false;
  }
  return true;
}

bool ResourceTableVisitor::visit(void* data, size_t len) {
  m_data = data;
  m_length = len;
  android::ResTable_header* table =
      convert_chunk<android::ResTable_header>((android::ResChunk_header*)data);
  if (!table) {
    ALOGE("corrupt ResTable_header chunk");
    return false;
  }
  return visit_table(table);
}

bool ResourceTableVisitor::visit_table(android::ResTable_header* table) {
  LOGVV("visit ResTable_header, offset = %ld", get_file_offset(table));
  android::ResStringPool_header* global_string_pool = nullptr;
  android::ResTable_package* package = nullptr;
  ResChunkPullParser parser(get_data(&table->header),
                            get_data_len(&table->header));
  while (ResChunkPullParser::IsGoodEvent(parser.Next())) {
    switch (dtohs(parser.chunk()->type)) {
    case android::RES_STRING_POOL_TYPE:
      if (global_string_pool == nullptr) {
        global_string_pool =
            convert_chunk<android::ResStringPool_header>(parser.chunk());
        if (global_string_pool == nullptr) {
          ALOGE("bad string pool chunk");
          return false;
        }
        if (!visit_global_strings(global_string_pool)) {
          return false;
        }
      } else {
        ALOGE("unexpected string pool in ResTable, ignoring");
      }
      break;
    case android::RES_TABLE_PACKAGE_TYPE:
      package = convert_chunk<android::ResTable_package, MIN_PACKAGE_SIZE>(
          parser.chunk());
      if (!valid(package)) {
        ALOGE("bad package chunk");
        return false;
      }
      if (!visit_package(package)) {
        return false;
      }
      break;
    default:
      ALOGE("unexpected chunk type %x, ignoring", dtohs(parser.chunk()->type));
      break;
    }
  }
  if (parser.event() == ResChunkPullParser::Event::BadDocument) {
    ALOGE("corrupt resource table");
    return false;
  }
  return true;
}

bool ResourceTableVisitor::visit_global_strings(
    android::ResStringPool_header* pool) {
  LOGVV("visit global string pool, offset = %ld", get_file_offset(pool));
  // Callers expected to override if inspecting strings/styles are required.
  return true;
}

bool ResourceTableVisitor::visit_package(android::ResTable_package* package) {
  LOGVV("visit ResTable_package, offset = %ld", get_file_offset(package));
  uint32_t package_id = dtohl(package->id);
  android::ResStringPool_header* type_strings = nullptr;
  android::ResStringPool_header* key_strings = nullptr;
  android::ResTable_typeSpec* type_spec = nullptr;
  android::ResTable_type* type = nullptr;
  ResChunkPullParser parser(get_data(&package->header),
                            get_data_len(&package->header));
  while (ResChunkPullParser::IsGoodEvent(parser.Next())) {
    switch (dtohs(parser.chunk()->type)) {
    case android::RES_STRING_POOL_TYPE:
      if (type_strings == nullptr) {
        type_strings =
            convert_chunk<android::ResStringPool_header>(parser.chunk());
        if (type_strings == nullptr) {
          ALOGE("bad string pool chunk");
          return false;
        }
        if (!visit_type_strings(package, type_strings)) {
          return false;
        }
      } else if (key_strings == nullptr) {
        key_strings =
            convert_chunk<android::ResStringPool_header>(parser.chunk());
        if (key_strings == nullptr) {
          ALOGE("bad string pool chunk");
          return false;
        }
        if (!visit_key_strings(package, key_strings)) {
          return false;
        }
      } else {
        ALOGE("unexpected string pool in package %x, ignoring", package_id);
      }
      break;
    case android::RES_TABLE_TYPE_SPEC_TYPE:
      type_spec = convert_chunk<android::ResTable_typeSpec>(parser.chunk());
      if (!valid(type_spec)) {
        ALOGE("bad type spec chunk");
        return false;
      }
      if (!visit_type_spec(package, type_spec)) {
        return false;
      }
      break;
    case android::RES_TABLE_TYPE_TYPE:
      type = convert_chunk<android::ResTable_type, MIN_RES_TABLE_TYPE_SIZE>(
          parser.chunk());
      if (!valid(type)) {
        ALOGE("bad type chunk");
        return false;
      }
      if (!visit_type(package, type_spec, type)) {
        return false;
      }
      break;
    default:
      auto unknown = parser.chunk();
      ALOGE("unexpected chunk type %x in package", dtohs(unknown->type));
      if (!visit_unknown_chunk(package, unknown)) {
        return false;
      }
      break;
    }
  }
  if (parser.event() == ResChunkPullParser::Event::BadDocument) {
    ALOGE("corrupt package %x", package_id);
    return false;
  }
  return true;
}

bool ResourceTableVisitor::visit_unknown_chunk(
  android::ResTable_package* package,
  android::ResChunk_header* header) {
  LOGVV("visit unknown chunk, offset = %ld", get_file_offset(header));
  return true;
}

bool ResourceTableVisitor::visit_type_strings(
    android::ResTable_package* package, android::ResStringPool_header* pool) {
  LOGVV("visit type strings, offset = %ld", get_file_offset(pool));
  // Callers expected to override if inspecting strings/styles are required.
  return true;
}

bool ResourceTableVisitor::visit_key_strings(
    android::ResTable_package* package, android::ResStringPool_header* pool) {
  LOGVV("visit key strings, offset = %ld", get_file_offset(pool));
  // Callers expected to override if inspecting strings/styles are required.
  return true;
}

bool ResourceTableVisitor::visit_type_spec(
    android::ResTable_package* package, android::ResTable_typeSpec* type_spec) {
  LOGVV("visit ResTable_typeSpec ID %x, offset = %ld",
        type_spec->id,
        get_file_offset(type_spec));
  return true;
}

bool ResourceTableVisitor::visit_type(android::ResTable_package* package,
                                      android::ResTable_typeSpec* type_spec,
                                      android::ResTable_type* type) {
  LOGVV("visit ResTable_type (of ResTable_typeSpec ID %x), offset = %ld",
        type_spec->id,
        get_file_offset(type));
  android::TypeVariant tv(type);
  for (auto it = tv.beginEntries(); it != tv.endEntries(); ++it) {
    android::ResTable_entry* entry = const_cast<android::ResTable_entry*>(*it);
    if (!entry) {
      continue;
    }
    if (dtohs(entry->flags) & android::ResTable_entry::FLAG_COMPLEX) {
      auto map_entry = static_cast<android::ResTable_map_entry*>(entry);
      auto entry_count = dtohl(map_entry->count);
      if (!visit_map_entry(package, type_spec, type, map_entry)) {
          return false;
        }
      for (size_t i = 0; i < entry_count; i++) {
        android::ResTable_map* value =
            (android::ResTable_map*)((uint8_t*)entry + dtohl(entry->size) +
                                     i * sizeof(android::ResTable_map));
        if (!visit_map_value(package, type_spec, type, map_entry, value)) {
          return false;
        }
      }
    } else {
      android::Res_value* value =
          (android::Res_value*)((uint8_t*)entry + dtohl(entry->size));
      if (!visit_entry(package, type_spec, type, entry, value)) {
        return false;
      }
    }
  }
  return true;
}

bool ResourceTableVisitor::visit_entry(android::ResTable_package* package,
                                       android::ResTable_typeSpec* type_spec,
                                       android::ResTable_type* type,
                                       android::ResTable_entry* entry,
                                       android::Res_value* value) {
  LOGVV("visit entry offset = %ld, value offset = %ld",
        get_file_offset(entry),
        get_file_offset(value));
  return true;
}

bool ResourceTableVisitor::visit_map_entry(
    android::ResTable_package* package,
    android::ResTable_typeSpec* type_spec,
    android::ResTable_type* type,
    android::ResTable_map_entry* entry) {
  LOGVV("visit map entry offset = %ld", get_file_offset(entry));
  return true;
}

bool ResourceTableVisitor::visit_map_value(
    android::ResTable_package* package,
    android::ResTable_typeSpec* type_spec,
    android::ResTable_type* type,
    android::ResTable_map_entry* entry,
    android::ResTable_map* value) {
  LOGVV("visit map value offset = %ld", get_file_offset(value));
  return true;
}

// BEGIN StringPoolRefVisitor

bool StringPoolRefVisitor::visit_key_strings_ref(
    android::ResTable_package* package, android::ResStringPool_ref* ref) {
  LOGVV("visit key ResStringPool_ref, offset = %ld", get_file_offset(ref));
  // Subclasses meant to override.
  return true;
}

bool StringPoolRefVisitor::visit_global_strings_ref(android::Res_value* value) {
  LOGVV("visit string Res_value, offset = %ld", get_file_offset(value));
  // Subclasses meant to override.
  return true;
}

bool StringPoolRefVisitor::visit_global_strings_ref(
    android::ResStringPool_ref* value) {
  LOGVV("visit global ResStringPool_ref, offset = %ld", get_file_offset(value));
  // Subclasses meant to override.
  return true;
}

bool StringPoolRefVisitor::visit_global_strings(
    android::ResStringPool_header* pool) {
  LOGVV("visit global string pool, offset = %ld", get_file_offset(pool));
  // Iterate ResStringPool_span items and their ResStringPool_refs (which point
  // to this pool).
  auto style_count = dtohl(pool->styleCount);
  auto styles_start = dtohl(pool->stylesStart);
  if (style_count > 0 && styles_start > 0) {
    auto style_idx =
        (uint32_t*)(((uint8_t*)pool) + dtohs(pool->header.headerSize) +
                    dtohl(pool->stringCount) * sizeof(uint32_t));
    for (size_t i = 0; i < style_count; i++) {
      auto span = (android::ResStringPool_span*)(((uint8_t*)pool) +
                                                 styles_start + *style_idx);
      while (dtohl(span->name.index) != android::ResStringPool_span::END) {
        LOGVV("visit ResStringPool_span, offset = %ld", get_file_offset(span));
        if (!visit_global_strings_ref(&span->name)) {
          return false;
        }
        span++;
      }
      style_idx++;
    }
  }
  return true;
}

bool StringPoolRefVisitor::visit_entry(android::ResTable_package* package,
                                       android::ResTable_typeSpec* type_spec,
                                       android::ResTable_type* type,
                                       android::ResTable_entry* entry,
                                       android::Res_value* value) {
  LOGVV("visit entry offset = %ld, value offset = %ld",
        get_file_offset(entry),
        get_file_offset(value));
  if (!visit_key_strings_ref(package, &entry->key)) {
    return false;
  }
  if (value->dataType == android::Res_value::TYPE_STRING) {
    if (!visit_global_strings_ref(value)) {
      return false;
    }
  }
  return true;
}

bool StringPoolRefVisitor::visit_map_entry(
    android::ResTable_package* package,
    android::ResTable_typeSpec* type_spec,
    android::ResTable_type* type,
    android::ResTable_map_entry* entry) {
  LOGVV("visit map entry offset = %ld", get_file_offset(entry));
  if (!visit_key_strings_ref(package, &entry->key)) {
    return false;
  }
  return true;
}

bool StringPoolRefVisitor::visit_map_value(
    android::ResTable_package* package,
    android::ResTable_typeSpec* type_spec,
    android::ResTable_type* type,
    android::ResTable_map_entry* entry,
    android::ResTable_map* value) {
  LOGVV("visit map value offset = %ld", get_file_offset(value));
  if (value->value.dataType == android::Res_value::TYPE_STRING) {
    if (!visit_global_strings_ref(&value->value)) {
      return false;
    }
  }
  return true;
}

} // namespace arsc
