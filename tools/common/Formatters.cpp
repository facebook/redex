/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Formatters.h"

#include "DexEncoding.h"

#include <iomanip>
#include <ios>
#include <sstream>

struct type_to_string {
  uint16_t type;
  const char* name;
};

#define GMAP_TYPE(NAME) \
  { NAME, #NAME }
type_to_string GMapTypes[] = {GMAP_TYPE(TYPE_HEADER_ITEM),
                              GMAP_TYPE(TYPE_STRING_ID_ITEM),
                              GMAP_TYPE(TYPE_TYPE_ID_ITEM),
                              GMAP_TYPE(TYPE_PROTO_ID_ITEM),
                              GMAP_TYPE(TYPE_FIELD_ID_ITEM),
                              GMAP_TYPE(TYPE_METHOD_ID_ITEM),
                              GMAP_TYPE(TYPE_CLASS_DEF_ITEM),
                              GMAP_TYPE(TYPE_MAP_LIST),
                              GMAP_TYPE(TYPE_TYPE_LIST),
                              GMAP_TYPE(TYPE_ANNOTATION_SET_REF_LIST),
                              GMAP_TYPE(TYPE_ANNOTATION_SET_ITEM),
                              GMAP_TYPE(TYPE_CLASS_DATA_ITEM),
                              GMAP_TYPE(TYPE_CODE_ITEM),
                              GMAP_TYPE(TYPE_STRING_DATA_ITEM),
                              GMAP_TYPE(TYPE_DEBUG_INFO_ITEM),
                              GMAP_TYPE(TYPE_ANNOTATION_ITEM),
                              GMAP_TYPE(TYPE_ENCODED_ARRAY_ITEM),
                              GMAP_TYPE(TYPE_ANNOTATIONS_DIR_ITEM),
                              GMAP_TYPE(TYPE_CALL_SITE_ID_ITEM),
                              GMAP_TYPE(TYPE_METHOD_HANDLE_ITEM),
                              {0, nullptr}};

/* nullptr terminated structure for looking up the name
 * of a map section. Think of it like ELF sections, but
 * lots of them for no apparent reason. Used for debugging
 * only.
 */
inline const char* maptype_to_string(uint16_t maptype) {
  type_to_string* p = GMapTypes;
  while (p->name != nullptr) {
    if (p->type == maptype) return p->name;
    p++;
  }
  return "Unknown";
}

std::string format_map(ddump_data* rd) {
  std::ostringstream ss;
  unsigned int count;
  dex_map_item* maps;
  get_dex_map_items(rd, &count, &maps);
  ss << "Type                              Size  Offset\n";
  for (unsigned int i = 0; i < count; i++) {
    ss << std::left << std::setfill(' ') << std::setw(30)
       << maptype_to_string(maps[i].type) << std::right << std::dec
       << std::setfill(' ') << std::setw(8) << maps[i].size << "  "
       << std::right << std::hex << std::setfill('0') << std::setw(8)
       << maps[i].offset << "\n";
  }
  return ss.str();
}

namespace {
const char* viz_to_string(uint8_t viz) {
  switch (viz) {
  case 0:
    return "BUILD";
  case 1:
    return "RUNTIME";
  case 2:
    return "SYSTEM";
  default:
    return "UNKNOWN_VIZ";
  };
}

const char* value_to_string(uint8_t value) {
  value &= 0x1f;
  switch (value) {
  case 0x00:
    return "BYTE";
  case 0x02:
    return "SHORT";
  case 0x03:
    return "CHAR";
  case 0x04:
    return "INT";
  case 0x06:
    return "LONG";
  case 0x10:
    return "FLOAT";
  case 0x11:
    return "DOUBLE";
  case 0x15:
    return "METHOD_TYPE";
  case 0x16:
    return "METHOD_HANDLE";
  case 0x17:
    return "STRING";
  case 0x18:
    return "TYPE";
  case 0x19:
    return "FIELD";
  case 0x1a:
    return "METHOD";
  case 0x1b:
    return "ENUM";
  case 0x1c:
    return "ARRAY";
  case 0x1d:
    return "ANNOTATION";
  case 0x1e:
    return "NULL";
  case 0x1f:
    return "BOOLEAN";
  default:
    return "UNKNOWN_VALUE";
  };
}

const char* s_empty_string = "";
const char* s_invalid_size = " *INVALID SIZE*";

const char* check_size(uint8_t value_type, uint8_t value_arg) {
  uint8_t valid_size = 0;
  switch (value_type) {
  case 0x00:
  case 0x1c:
  case 0x1d:
  case 0x1e:
    valid_size = 1;
    break;
  case 0x02:
  case 0x03:
  case 0x1f:
    valid_size = 2;
    break;
  case 0x04:
  case 0x10:
  case 0x15:
  case 0x16:
  case 0x17:
  case 0x18:
  case 0x19:
  case 0x1a:
  case 0x1b:
    valid_size = 4;
    break;
  case 0x06:
  case 0x11:
    valid_size = 8;
    break;
  }
  return (value_arg < valid_size) ? s_empty_string : s_invalid_size;
}
} // namespace

std::string format_encoded_value(ddump_data* rd, const uint8_t** _aitem) {
  std::ostringstream ss;
  const uint8_t* aitem = *_aitem;
  uint8_t value = *aitem++;
  uint8_t upperbits = value >> 5;
  value &= 0x1f;
  switch (value) {
  case 0x00:
  case 0x02:
  case 0x03:
  case 0x04:
  case 0x06:
  case 0x10:
  case 0x11:
  case 0x16:
  case 0x19:
  case 0x1a:
  case 0x1b: {
    ss << "[" << value_to_string(value) << check_size(value, upperbits) << " "
       << std::hex << std::setfill('0') << std::setw(2) << uint32_t(*aitem++);
    while (upperbits--) {
      ss << " " << std::hex << std::setfill('0') << std::setw(2)
         << uint32_t(*aitem++);
    }
    ss << "]";
    break;
  }
  case 0x15: {
    char* rtype = nullptr;
    uint32_t protoidx = uint32_t(*aitem++);
    dex_proto_id* proto = rd->dex_proto_ids + protoidx;
    if (proto->rtypeidx) rtype = dex_string_by_type_idx(rd, proto->rtypeidx);
    ss << "[METHOD_TYPE " << rtype << "(";
    if (proto->param_off) {
      uint32_t* tl = (uint32_t*)(rd->dexmmap + proto->param_off);
      int count = (int)*tl++;
      uint16_t* types = (uint16_t*)tl;
      for (int i = 0; i < count; i++) {
        char* strtype = dex_string_by_type_idx(rd, *types++);
        if (i == 0) {
          ss << strtype;
        } else {
          ss << " " << strtype;
        }
      }
    }
    ss << ")]";

    break;
  }
  case 0x18: {
    uint32_t typeidx;
    uint32_t shift = 8;
    typeidx = *aitem++;
    while (upperbits--) {
      typeidx |= *aitem++ << shift;
      shift += 8;
    }
    ss << "[TYPE '" << dex_string_by_type_idx(rd, typeidx) << "']";
    break;
  }
  case 0x17: {
    uint32_t stridx;
    uint32_t shift = 8;
    stridx = *aitem++;
    while (upperbits--) {
      stridx |= *aitem++ << shift;
      shift += 8;
    }
    ss << "[STRING '" << dex_string_by_idx(rd, stridx) << "']";
    break;
  }
  case 0x1c: {
    uint32_t size = read_uleb128(&aitem);
    ss << "[ARRAY ";
    while (size--) {
      ss << format_encoded_value(rd, &aitem);
    }
    ss << "]";
    break;
  }
  case 0x1d:
    ss << "[ANNOTATION " << format_annotation(rd, &aitem) << "]";
    break;
  case 0x1e:
    ss << "[NULL]";
    break;
  case 0x1f:
    ss << "[BOOL " << (upperbits ? "TRUE" : "FALSE") << "]";
    break;
  default:
    ss << "[UNKNOWN_VALUE]";
  };
  *_aitem = aitem;
  return ss.str();
}

std::string format_callsite(ddump_data* rd, const uint8_t** _aitem) {
  std::ostringstream ss;
  const uint8_t* aitem = *_aitem;
  uint32_t size = read_uleb128(&aitem);
  ss << " args: " << size;
  ss << " method: " << format_encoded_value(rd, &aitem);
  ss << " name: " << format_encoded_value(rd, &aitem);
  ss << " type: " << format_encoded_value(rd, &aitem);
  return ss.str();
}

std::string format_annotation(ddump_data* rd, const uint8_t** _aitem) {
  std::ostringstream ss;
  const uint8_t* aitem = *_aitem;
  uint32_t type_idx = read_uleb128(&aitem);
  uint32_t size = read_uleb128(&aitem);
  char* tstring = dex_string_by_type_idx(rd, type_idx);
  ss << tstring << "\n";
  while (size--) {
    uint32_t name_idx = read_uleb128(&aitem);
    char* key = dex_string_by_idx(rd, name_idx);
    ss << "            " << key << ":" << format_encoded_value(rd, &aitem)
       << "\n";
  }
  *_aitem = aitem;
  return ss.str();
}

std::string format_annotation_item(ddump_data* rd, const uint8_t** _aitem) {
  std::ostringstream ss;
  const uint8_t* aitem = *_aitem;
  uint8_t viz = *aitem++;
  *_aitem = aitem;
  auto anno = format_annotation(rd, _aitem);
  ss << "        Vis: " << viz_to_string(viz) << ", " << anno;
  return ss.str();
}

std::string format_method(ddump_data* rd, int idx) {
  std::ostringstream ss;
  dex_method_id* method = rd->dex_method_ids + idx;
  char* type = nullptr;
  char* name = nullptr;
  if (method->classidx) type = dex_string_by_type_idx(rd, method->classidx);
  ss << "type: " << type << " ";
  char* rtype = nullptr;
  if (method->protoidx) {
    dex_proto_id* proto = rd->dex_proto_ids + method->protoidx;
    if (proto->rtypeidx) rtype = dex_string_by_type_idx(rd, proto->rtypeidx);
    ss << "proto: rtype " << rtype << " args(";
    if (proto->param_off) {
      uint32_t* tl = (uint32_t*)(rd->dexmmap + proto->param_off);
      int count = (int)*tl++;
      uint16_t* types = (uint16_t*)tl;
      for (int i = 0; i < count; i++) {
        char* strtype = dex_string_by_type_idx(rd, *types++);
        if (i == 0) {
          ss << strtype;
        } else {
          ss << " " << strtype;
        }
      }
    }
    ss << ") ";
  }
  if (method->nameidx) name = dex_string_by_idx(rd, method->nameidx);
  ss << "name: " << name << "\n";
  return ss.str();
}

#define MHT_CASE(x) \
  case x:           \
    return #x;

std::string format_method_handle_type(MethodHandleType type) {
  switch (type) {
    MHT_CASE(METHOD_HANDLE_TYPE_STATIC_PUT)
    MHT_CASE(METHOD_HANDLE_TYPE_STATIC_GET)
    MHT_CASE(METHOD_HANDLE_TYPE_INSTANCE_PUT)
    MHT_CASE(METHOD_HANDLE_TYPE_INSTANCE_GET)
    MHT_CASE(METHOD_HANDLE_TYPE_INVOKE_STATIC)
    MHT_CASE(METHOD_HANDLE_TYPE_INVOKE_INSTANCE)
    MHT_CASE(METHOD_HANDLE_TYPE_INVOKE_CONSTRUCTOR)
    MHT_CASE(METHOD_HANDLE_TYPE_INVOKE_DIRECT)
    MHT_CASE(METHOD_HANDLE_TYPE_INVOKE_INTERFACE)
  default:
    return "INVALID METHOD HANDLE TYPE";
  }
}
