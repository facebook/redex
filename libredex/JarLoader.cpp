/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/iostreams/device/mapped_file.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <utility>
#include <vector>
#include <zlib.h>

#include "Macros.h"

#if IS_WINDOWS
#include <Winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <netinet/in.h>
#endif

#include "Creators.h"
#include "DexClass.h"
#include "DuplicateClasses.h"
#include "JarLoader.h"
#include "Show.h"
#include "Trace.h"
#include "Util.h"

/******************
 * Begin Class Loading code.
 */

namespace JarLoaderUtil {
uint32_t read32(uint8_t*& buffer, uint8_t* buffer_end) {
  uint32_t rv;
  auto next = buffer + sizeof(uint32_t);
  if (next > buffer_end) {
    throw RedexException(RedexError::BUFFER_END_EXCEEDED);
  }
  memcpy(&rv, buffer, sizeof(uint32_t));
  buffer = next;
  return htonl(rv);
}

uint16_t read16(uint8_t*& buffer, uint8_t* buffer_end) {
  uint16_t rv;
  auto next = buffer + sizeof(uint16_t);
  if (next > buffer_end) {
    throw RedexException(RedexError::BUFFER_END_EXCEEDED);
  }
  memcpy(&rv, buffer, sizeof(uint16_t));
  buffer = next;
  return htons(rv);
}

uint8_t read8(uint8_t*& buffer, uint8_t* buffer_end) {
  if (buffer >= buffer_end) {
    throw RedexException(RedexError::BUFFER_END_EXCEEDED);
  }
  return *buffer++;
}
} // namespace JarLoaderUtil

using namespace JarLoaderUtil;

namespace {

constexpr uint32_t kClassMagic = 0xcafebabe;

struct cp_entry {
  uint8_t tag;
  union {
    struct {
      uint16_t s0;
      uint16_t s1;
    };
    struct {
      uint32_t i0;
      uint32_t i1;
    };
    struct {
      uint16_t len;
      uint8_t* data;
    };
  };
};

struct cp_field_info {
  uint16_t aflags;
  uint16_t nameNdx;
  uint16_t descNdx;
};
struct cp_method_info {
  uint16_t aflags;
  uint16_t nameNdx;
  uint16_t descNdx;
};

/* clang-format off */

// Java Virtual Machine Specification Chapter 4, Section 4.4
constexpr size_t CP_CONST_UTF8 =         1;
constexpr size_t CP_CONST_INT =          3;
constexpr size_t CP_CONST_FLOAT =        4;
constexpr size_t CP_CONST_LONG =         5;
constexpr size_t CP_CONST_DOUBLE =       6;
constexpr size_t CP_CONST_CLASS =        7;
constexpr size_t CP_CONST_STRING =       8;
constexpr size_t CP_CONST_FIELD =        9;
constexpr size_t CP_CONST_METHOD =      10;
constexpr size_t CP_CONST_INTERFACE =   11;
constexpr size_t CP_CONST_NAMEANDTYPE = 12;

// Since Java 7
constexpr size_t CP_CONST_METHHANDLE =  15;
constexpr size_t CP_CONST_METHTYPE =    16;
constexpr size_t CP_CONST_INVOKEDYN =   18;

// Since Java 9
constexpr size_t CP_CONST_MODULE =      19;
constexpr size_t CP_CONST_PACKAGE =     20;

/* clang-format on */

bool parse_cp_entry(uint8_t*& buffer, uint8_t* buffer_end, cp_entry& cpe) {
  cpe.tag = read8(buffer, buffer_end);
  switch (cpe.tag) {
  case CP_CONST_CLASS:
  case CP_CONST_STRING:
  case CP_CONST_METHTYPE:
  case CP_CONST_MODULE:
  case CP_CONST_PACKAGE:
    cpe.s0 = read16(buffer, buffer_end);
    return true;
  case CP_CONST_FIELD:
  case CP_CONST_METHOD:
  case CP_CONST_INTERFACE:
  case CP_CONST_NAMEANDTYPE:
    cpe.s0 = read16(buffer, buffer_end);
    cpe.s1 = read16(buffer, buffer_end);
    return true;
  case CP_CONST_METHHANDLE:
    cpe.s0 = read8(buffer, buffer_end);
    cpe.s1 = read16(buffer, buffer_end);
    return true;
  case CP_CONST_INT:
  case CP_CONST_FLOAT:
    cpe.i0 = read32(buffer, buffer_end);
    return true;
  case CP_CONST_LONG:
  case CP_CONST_DOUBLE:
    cpe.i0 = read32(buffer, buffer_end);
    cpe.i1 = read32(buffer, buffer_end);
    return true;
  case CP_CONST_UTF8:
    cpe.len = read16(buffer, buffer_end);
    cpe.data = buffer;
    buffer += cpe.len;
    if (buffer > buffer_end) {
      throw RedexException(RedexError::BUFFER_END_EXCEEDED);
    }
    return true;
  case CP_CONST_INVOKEDYN:
    std::cerr << "INVOKEDYN constant unsupported, Bailing\n";
    return false;
  }
  std::cerr << "Unrecognized constant pool tag 0x" << std::hex << cpe.tag
            << ", Bailing\n";
  return false;
}

void skip_attributes(uint8_t*& buffer, uint8_t* buffer_end) {
  /* Todo:
   * Consider adding some verification so we don't walk
   * off the end in the case of a corrupt class file.
   */
  uint16_t acount = read16(buffer, buffer_end);
  for (int i = 0; i < acount; i++) {
    buffer += 2; // Skip name_index
    uint32_t length = read32(buffer, buffer_end);
    buffer += length;
  }
}

constexpr size_t MAX_CLASS_NAMELEN = 8 * 1024;

DexType* make_dextype_from_cref(std::vector<cp_entry>& cpool, uint16_t cref) {
  char nbuffer[MAX_CLASS_NAMELEN];
  if (cref >= cpool.size()) {
    std::cerr << "Illegal cref, Bailing\n";
    return nullptr;
  }
  if (cpool[cref].tag != CP_CONST_CLASS) {
    std::cerr << "Non-class ref in get_class_name, Bailing\n";
    return nullptr;
  }
  uint16_t utf8ref = cpool[cref].s0;
  const cp_entry& utf8cpe = cpool[utf8ref];
  if (utf8cpe.tag != CP_CONST_UTF8) {
    std::cerr << "Non-utf8 ref in get_utf8, Bailing\n";
    return nullptr;
  }
  if (utf8cpe.len > (MAX_CLASS_NAMELEN + 3)) {
    std::cerr << "classname is greater than max, bailing";
    return nullptr;
  }
  nbuffer[0] = 'L';
  memcpy(nbuffer + 1, utf8cpe.data, utf8cpe.len);
  nbuffer[1 + utf8cpe.len] = ';';
  nbuffer[2 + utf8cpe.len] = '\0';
  return DexType::make_type(nbuffer);
}

bool extract_utf8(std::vector<cp_entry>& cpool,
                  uint16_t utf8ref,
                  std::string_view* out) {
  if (utf8ref >= cpool.size()) {
    std::cerr << "utf8 ref out of bound, bailing\n";
    return false;
  }
  const cp_entry& utf8cpe = cpool[utf8ref];
  if (utf8cpe.tag != CP_CONST_UTF8) {
    std::cerr << "Non-utf8 ref in get_utf8, bailing\n";
    return false;
  }
  if (utf8cpe.len > (MAX_CLASS_NAMELEN - 1)) {
    std::cerr << "Name is greater (" << utf8cpe.len << ") than max ("
              << MAX_CLASS_NAMELEN << "), bailing\n";
    return false;
  }
  *out = std::string_view((const char*)utf8cpe.data, utf8cpe.len);
  return true;
}

DexField* make_dexfield(std::vector<cp_entry>& cpool,
                        DexType* self,
                        cp_field_info& finfo) {
  std::string_view dbuffer;
  std::string_view nbuffer;
  if (!extract_utf8(cpool, finfo.nameNdx, &nbuffer) ||
      !extract_utf8(cpool, finfo.descNdx, &dbuffer)) {
    return nullptr;
  }
  auto name = DexString::make_string(nbuffer);
  DexType* desc = DexType::make_type(dbuffer);
  DexField* field =
      static_cast<DexField*>(DexField::make_field(self, name, desc));
  field->set_access((DexAccessFlags)finfo.aflags);
  field->set_external();
  return field;
}

DexType* sSimpleTypeB;
DexType* sSimpleTypeC;
DexType* sSimpleTypeD;
DexType* sSimpleTypeF;
DexType* sSimpleTypeI;
DexType* sSimpleTypeJ;
DexType* sSimpleTypeS;
DexType* sSimpleTypeZ;
DexType* sSimpleTypeV;

} // namespace

void init_basic_types() {
  sSimpleTypeB = DexType::make_type("B");
  sSimpleTypeC = DexType::make_type("C");
  sSimpleTypeD = DexType::make_type("D");
  sSimpleTypeF = DexType::make_type("F");
  sSimpleTypeI = DexType::make_type("I");
  sSimpleTypeJ = DexType::make_type("J");
  sSimpleTypeS = DexType::make_type("S");
  sSimpleTypeZ = DexType::make_type("Z");
  sSimpleTypeV = DexType::make_type("V");
}

namespace {

DexType* parse_type(std::string_view& buf) {
  char typebuffer[MAX_CLASS_NAMELEN];
  char desc = buf.at(0);
  buf = buf.substr(1);
  switch (desc) {
  case 'B':
    return sSimpleTypeB;
  case 'C':
    return sSimpleTypeC;
  case 'D':
    return sSimpleTypeD;
  case 'F':
    return sSimpleTypeF;
  case 'I':
    return sSimpleTypeI;
  case 'J':
    return sSimpleTypeJ;
  case 'S':
    return sSimpleTypeS;
  case 'Z':
    return sSimpleTypeZ;
  case 'V':
    return sSimpleTypeV;
  case 'L': {
    char* tpout = typebuffer;
    *tpout++ = desc;
    while (buf.at(0) != ';') {
      *tpout++ = buf[0];
      buf = buf.substr(1);
    }
    *tpout++ = buf.at(0);
    buf = buf.substr(1);
    *tpout = '\0';
    return DexType::make_type(typebuffer);
    break;
  }
  case '[': {
    char* tpout = typebuffer;
    *tpout++ = desc;
    while (buf.at(0) == '[') {
      *tpout++ = buf[0];
      buf = buf.substr(1);
    }
    if (buf.at(0) == 'L') {
      while (buf.at(0) != ';') {
        *tpout++ = buf[0];
        buf = buf.substr(1);
      }
      *tpout++ = buf.at(0);
      buf = buf.substr(1);
    } else {
      *tpout++ = buf[0];
      buf = buf.substr(1);
    }
    *tpout++ = '\0';
    return DexType::make_type(typebuffer);
  }
  }
  std::cerr << "Invalid parse-type '" << desc << "', bailing\n";
  return nullptr;
}

DexTypeList* extract_arguments(std::string_view& buf) {
  buf = buf.substr(1);
  if (buf.at(0) == ')') {
    buf = buf.substr(1);
    return DexTypeList::make_type_list({});
  }
  DexTypeList::ContainerType args;
  while (buf.at(0) != ')') {
    DexType* dtype = parse_type(buf);
    if (dtype == nullptr) return nullptr;
    if (dtype == sSimpleTypeV) {
      std::cerr << "Invalid argument type 'V' in args, bailing\n";
      return nullptr;
    }
    args.push_back(dtype);
  }
  buf = buf.substr(1);
  return DexTypeList::make_type_list(std::move(args));
}

DexMethod* make_dexmethod(std::vector<cp_entry>& cpool,
                          DexType* self,
                          cp_method_info& finfo) {
  std::string_view dbuffer;
  std::string_view nbuffer;
  if (!extract_utf8(cpool, finfo.nameNdx, &nbuffer) ||
      !extract_utf8(cpool, finfo.descNdx, &dbuffer)) {
    return nullptr;
  }
  auto name = DexString::make_string(nbuffer);
  std::string_view ptr = dbuffer;
  DexTypeList* tlist = extract_arguments(ptr);
  if (tlist == nullptr) return nullptr;
  DexType* rtype = parse_type(ptr);
  if (rtype == nullptr) return nullptr;
  DexProto* proto = DexProto::make_proto(rtype, tlist);
  DexMethod* method =
      static_cast<DexMethod*>(DexMethod::make_method(self, name, proto));
  if (method->is_concrete()) {
    std::cerr << "Pre-concrete method attempted to load '" << show(method)
              << "', bailing\n";
    return nullptr;
  }
  uint32_t access = finfo.aflags;
  bool is_virt = true;
  if (nbuffer[0] == '<') {
    is_virt = false;
    if (nbuffer[1] == 'i') {
      access |= ACC_CONSTRUCTOR;
    }
  } else if (access & (ACC_PRIVATE | ACC_STATIC))
    is_virt = false;
  method->set_access((DexAccessFlags)access);
  method->set_virtual(is_virt);
  method->set_external();
  return method;
}

} // namespace

bool parse_class(uint8_t* buffer,
                 size_t buffer_size,
                 Scope* classes,
                 attribute_hook_t attr_hook,
                 const jar_loader::duplicate_allowed_hook_t& is_allowed,
                 const DexLocation* jar_location) {
  auto buffer_end = buffer + buffer_size;
  uint32_t magic = read32(buffer, buffer_end);
  uint16_t vminor DEBUG_ONLY = read16(buffer, buffer_end);
  uint16_t vmajor DEBUG_ONLY = read16(buffer, buffer_end);
  uint16_t cp_count = read16(buffer, buffer_end);
  if (magic != kClassMagic) {
    std::cerr << "Bad class magic " << std::hex << magic << ", Bailing\n";
    return false;
  }
  std::vector<cp_entry> cpool;
  cpool.resize(cp_count);
  /* The zero'th entry is always empty.  Java is annoying. */
  for (int i = 1; i < cp_count; i++) {
    if (!parse_cp_entry(buffer, buffer_end, cpool[i])) return false;
    if (cpool[i].tag == CP_CONST_LONG || cpool[i].tag == CP_CONST_DOUBLE) {
      cpool[i + 1] = cpool[i];
      i++;
    }
  }
  uint16_t aflags = read16(buffer, buffer_end);
  uint16_t clazz = read16(buffer, buffer_end);
  uint16_t super = read16(buffer, buffer_end);
  uint16_t ifcount = read16(buffer, buffer_end);

  if (is_module((DexAccessFlags)aflags)) {
    // Classes with the ACC_MODULE access flag are special.  They contain
    // metadata for the module/package system and don't have a superclass.
    // Ignore them for now.
    TRACE(MAIN, 5, "Warning: ignoring module-info class in jar '%s'",
          jar_location->get_file_name().c_str());
    return true;
  }

  DexType* self = make_dextype_from_cref(cpool, clazz);
  if (self == nullptr) {
    std::cerr << "Bad class cpool index " << clazz << ", Bailing\n";
    return false;
  }
  DexClass* cls = type_class(self);
  if (cls) {
    // We are seeing duplicate classes when parsing jar file
    if (cls->is_external()) {
      // Two external classes in .jar file has the same name
      // Just issue an warning for now
      TRACE(MAIN, 1,
            "Warning: Found a duplicate class '%s' in two .jar files:\n "
            "  Current: '%s'\n"
            "  Previous: '%s'",
            SHOW(self), jar_location->get_file_name().c_str(),
            cls->get_location()->get_file_name().c_str());
    } else if (!dup_classes::is_known_dup(cls)) {
      TRACE(MAIN, 1,
            "Warning: Found a duplicate class '%s' in .dex and .jar file."
            "  Current: '%s'\n"
            "  Previous: '%s'\n",
            SHOW(self), jar_location->get_file_name().c_str(),
            cls->get_location()->get_file_name().c_str());

      if (!is_allowed(cls, jar_location->get_file_name())) {
        throw RedexException(RedexError::DUPLICATE_CLASSES,
                             "Found duplicate class in two different files.",
                             {{"class", SHOW(self)},
                              {"jar", jar_location->get_file_name()},
                              {"dex", cls->get_location()->get_file_name()}});
      }
    }
    return true;
  }

  ClassCreator cc(self, jar_location);
  cc.set_external();
  if (super != 0) {
    DexType* sclazz = make_dextype_from_cref(cpool, super);
    if (self == nullptr) {
      std::cerr << "Bad super class cpool index " << super << ", Bailing\n";
      return false;
    }
    cc.set_super(sclazz);
  }
  cc.set_access((DexAccessFlags)aflags);
  if (ifcount) {
    for (int i = 0; i < ifcount; i++) {
      uint16_t iface = read16(buffer, buffer_end);
      DexType* iftype = make_dextype_from_cref(cpool, iface);
      cc.add_interface(iftype);
    }
  }
  uint16_t fcount = read16(buffer, buffer_end);

  auto invoke_attr_hook =
      [&](const boost::variant<DexField*, DexMethod*>& field_or_method,
          uint8_t* attrPtr) {
        if (attr_hook == nullptr) {
          return;
        }
        uint16_t attributes_count = read16(attrPtr, buffer_end);
        for (uint16_t j = 0; j < attributes_count; j++) {
          uint16_t attribute_name_index = read16(attrPtr, buffer_end);
          uint32_t attribute_length = read32(attrPtr, buffer_end);
          std::string_view attribute_name;
          auto extract_res =
              extract_utf8(cpool, attribute_name_index, &attribute_name);
          always_assert_log(
              extract_res,
              "attribute hook was specified, but failed to load the attribute "
              "name due to insufficient name buffer");
          attr_hook(field_or_method, attribute_name, attrPtr, buffer_end);
          attrPtr += attribute_length;
        }
      };

  for (int i = 0; i < fcount; i++) {
    cp_field_info cpfield;
    cpfield.aflags = read16(buffer, buffer_end);
    cpfield.nameNdx = read16(buffer, buffer_end);
    cpfield.descNdx = read16(buffer, buffer_end);
    uint8_t* attrPtr = buffer;
    skip_attributes(buffer, buffer_end);
    DexField* field = make_dexfield(cpool, self, cpfield);
    if (field == nullptr) return false;
    cc.add_field(field);
    invoke_attr_hook({field}, attrPtr);
  }

  uint16_t mcount = read16(buffer, buffer_end);
  if (mcount) {
    for (int i = 0; i < mcount; i++) {
      cp_method_info cpmethod;
      cpmethod.aflags = read16(buffer, buffer_end);
      cpmethod.nameNdx = read16(buffer, buffer_end);
      cpmethod.descNdx = read16(buffer, buffer_end);

      uint8_t* attrPtr = buffer;
      skip_attributes(buffer, buffer_end);
      DexMethod* method = make_dexmethod(cpool, self, cpmethod);
      if (method == nullptr) return false;
      cc.add_method(method);
      invoke_attr_hook({method}, attrPtr);
    }
  }
  DexClass* dc = cc.create();
  if (classes != nullptr) {
    classes->emplace_back(dc);
  }

  constexpr bool kDebugPrint = false;
  if (kDebugPrint) {
    fprintf(stderr, "DexClass constructed from jar:\n%s\n", SHOW(dc));
    if (!dc->get_sfields().empty()) {
      fprintf(stderr, "Static Fields:\n");
      for (auto const& field : dc->get_sfields()) {
        fprintf(stderr, "\t%s\n", SHOW(field));
      }
    }
    if (!dc->get_ifields().empty()) {
      fprintf(stderr, "Instance Fields:\n");
      for (auto const& field : dc->get_ifields()) {
        fprintf(stderr, "\t%s\n", SHOW(field));
      }
    }
    if (!dc->get_dmethods().empty()) {
      fprintf(stderr, "Direct Methods:\n");
      for (auto const& method : dc->get_dmethods()) {
        fprintf(stderr, "\t%s\n", SHOW(method));
      }
    }
    if (!dc->get_vmethods().empty()) {
      fprintf(stderr, "Virtual Methods:\n");
      for (auto const& method : dc->get_vmethods()) {
        fprintf(stderr, "\t%s\n", SHOW(method));
      }
    }
  }

  return true;
}

bool load_class_file(const std::string& filename, Scope* classes) {
  // It's not exactly efficient to call init_basic_types repeatedly for each
  // class file that we load, but load_class_file should typically only be used
  // in tests to load a small number of files.
  init_basic_types();

  std::ifstream ifs(filename, std::ifstream::binary);
  auto buf = ifs.rdbuf();
  size_t size = buf->pubseekoff(0, ifs.end, ifs.in);
  buf->pubseekpos(0, ifs.in);
  auto buffer = std::make_unique<char[]>(size);
  buf->sgetn(buffer.get(), size);
  auto jar_location = DexLocation::make_location("", filename);
  return parse_class(
      reinterpret_cast<uint8_t*>(buffer.get()), size, classes,
      /* attr_hook */ nullptr,
      /* is_allowed=*/[](auto*, auto&) { return true; }, jar_location);
}

/******************
 * Begin Jar Loading code.
 *
 */

namespace {

/* CDFile
 * Central directory file header entry structures.
 */
constexpr uint16_t kCompMethodStore = 0;
constexpr uint16_t kCompMethodDeflate = 8;
constexpr std::array<uint8_t, 4> kCDFile = {'P', 'K', 0x01, 0x02};

PACKED(struct pk_cd_file {
  uint32_t signature;
  uint16_t vmade;
  uint16_t vextract;
  uint16_t flags;
  uint16_t comp_method;
  uint16_t mod_time;
  uint16_t mod_date;
  uint32_t crc32;
  uint32_t comp_size;
  uint32_t ucomp_size;
  uint16_t fname_len;
  uint16_t extra_len;
  uint16_t comment_len;
  uint16_t diskno;
  uint16_t interal_attr;
  uint32_t external_attr;
  uint32_t disk_offset;
});

/* CDirEnd:
 * End of central directory record structures.
 */
constexpr int kMaxCDirEndSearch = 100;
constexpr std::array<uint8_t, 4> kCDirEnd = {'P', 'K', 0x05, 0x06};

PACKED(struct pk_cdir_end {
  uint32_t signature;
  uint16_t diskno;
  uint16_t cd_diskno;
  uint16_t cd_disk_entries;
  uint16_t cd_entries;
  uint32_t cd_size;
  uint32_t cd_disk_offset;
  uint16_t comment_len;
});

/* LFile:
 * Local file header structures.
 * (Yes, this made more sense in the world of floppies and tapes.)
 */
constexpr std::array<uint8_t, 4> kLFile = {'P', 'K', 0x03, 0x04};

PACKED(struct pk_lfile {
  uint32_t signature;
  uint16_t vextract;
  uint16_t flags;
  uint16_t comp_method;
  uint16_t mod_time;
  uint16_t mod_date;
  uint32_t crc32;
  uint32_t comp_size;
  uint32_t ucomp_size;
  uint16_t fname_len;
  uint16_t extra_len;
});

struct jar_entry {
  struct pk_cd_file cd_entry;
  std::string filename;
};

bool find_central_directory(const uint8_t* mapping,
                            ssize_t size,
                            pk_cdir_end& pce) {
  ssize_t soffset = (size - sizeof(pk_cdir_end));
  ssize_t eoffset = soffset - kMaxCDirEndSearch;
  if (soffset < 0) return false;
  if (eoffset < 0) eoffset = 0;
  do {
    const uint8_t* cdsearch = mapping + soffset;
    if (memcmp(cdsearch, kCDirEnd.data(), kCDirEnd.size()) == 0) {
      memcpy(&pce, cdsearch, sizeof(pk_cdir_end));
      return true;
    }
  } while (soffset-- > eoffset);
  std::cerr << "End of central directory record not found, bailing\n";
  return false;
}

bool validate_pce(pk_cdir_end& pce, ssize_t size) {
  /* We only support a limited feature set.  We
   * don't support disk-spanning, so bail if that's the case.
   */
  if (pce.cd_diskno != pce.diskno || pce.cd_diskno != 0 ||
      pce.cd_entries != pce.cd_disk_entries) {
    std::cerr << "Disk spanning is not supported, bailing\n";
    return false;
  }
  ssize_t data_size = size - sizeof(pk_cdir_end);
  if (pce.cd_disk_offset + pce.cd_size > data_size) {
    std::cerr << "Central directory overflow, invalid pce structure\n";
    return false;
  }
  return true;
}

bool extract_jar_entry(const uint8_t*& mapping,
                       size_t& offset,
                       size_t total_size,
                       jar_entry& je) {
  if (offset + kCDFile.size() > total_size) {
    std::cerr << "Reading mapping out of bound, bailing\n";
    return false;
  }
  if (memcmp(mapping, kCDFile.data(), kCDFile.size()) != 0) {
    std::cerr << "Invalid central directory entry, bailing\n";
    return false;
  }
  offset += sizeof(pk_cd_file);
  always_assert_log(offset < total_size, "Reading mapping out of bound");
  memcpy(&je.cd_entry, mapping, sizeof(pk_cd_file));
  mapping += sizeof(pk_cd_file);
  offset += je.cd_entry.fname_len;
  always_assert_log(offset < total_size, "Reading mapping out of bound");
  je.filename = std::string((const char*)mapping, je.cd_entry.fname_len);
  mapping += je.cd_entry.fname_len;
  offset = offset + je.cd_entry.extra_len + je.cd_entry.comment_len;
  mapping += je.cd_entry.extra_len;
  mapping += je.cd_entry.comment_len;
  return true;
}

bool get_jar_entries(const uint8_t* mapping,
                     size_t size,
                     pk_cdir_end& pce,
                     std::vector<jar_entry>& files) {
  const uint8_t* cdir = mapping + pce.cd_disk_offset;
  files.resize(pce.cd_entries);
  size_t offset = pce.cd_disk_offset;
  for (int entry = 0; entry < pce.cd_entries; entry++) {
    if (!extract_jar_entry(cdir, offset, size, files[entry])) return false;
  }
  return true;
}

int jar_uncompress(Bytef* dest,
                   uLongf* destLen,
                   const Bytef* source,
                   uLong sourceLen,
                   uint32_t comp_method) {
  if (comp_method == kCompMethodStore) {
    if (sourceLen > *destLen) {
      std::cerr << "Not enough space for STOREd entry: " << sourceLen << " vs "
                << *destLen << std::endl;
      return Z_BUF_ERROR;
    }
    memcpy(dest, source, sourceLen);
    *destLen = sourceLen;
    return Z_OK;
  }

  z_stream stream;
  int err;

  stream.next_in = (Bytef*)source;
  stream.avail_in = (uInt)sourceLen;
  stream.next_out = dest;
  stream.avail_out = (uInt)*destLen;
  stream.zalloc = (alloc_func)0;
  stream.zfree = (free_func)0;

  err = inflateInit2(&stream, -MAX_WBITS);
  if (err != Z_OK) return err;

  err = inflate(&stream, Z_FINISH);
  if (err != Z_STREAM_END) {
    inflateEnd(&stream);
    return err;
  }
  *destLen = stream.total_out;

  err = inflateEnd(&stream);
  return err;
}

bool decompress_class(jar_entry& file,
                      const uint8_t* mapping,
                      size_t map_size,
                      uint8_t* outbuffer,
                      ssize_t bufsize) {
  if (file.cd_entry.comp_method != kCompMethodDeflate &&
      file.cd_entry.comp_method != kCompMethodStore) {
    std::cerr << "Unknown compression method " << file.cd_entry.comp_method
              << " for " << file.filename << ", Bailing\n";
    return false;
  }

  static_assert(kLFile.size() <= sizeof(pk_lfile));
  if (file.cd_entry.disk_offset + sizeof(pk_lfile) >= map_size) {
    std::cerr << "Entry out of map bounds!\n";
    return false;
  }
  const uint8_t* lfile = mapping + file.cd_entry.disk_offset;
  if (memcmp(lfile, kLFile.data(), kLFile.size()) != 0) {
    std::cerr << "Invalid local file entry, bailing\n";
    return false;
  }

  pk_lfile pkf;
  memcpy(&pkf, lfile, sizeof(pk_lfile));
  if (pkf.comp_size == 0 && pkf.ucomp_size == 0 &&
      pkf.comp_size != file.cd_entry.comp_size &&
      pkf.ucomp_size != file.cd_entry.ucomp_size) {
    pkf.comp_size = file.cd_entry.comp_size;
    pkf.ucomp_size = file.cd_entry.ucomp_size;
  }

  lfile += sizeof(pk_lfile);

  if (file.cd_entry.disk_offset + sizeof(pk_lfile) + pkf.fname_len +
          pkf.extra_len + pkf.comp_size >=
      map_size) {
    std::cerr << "Complete entry exceeds mapping bounds.\n";
    return false;
  }

  if (pkf.fname_len != file.cd_entry.fname_len ||
      pkf.comp_size != file.cd_entry.comp_size ||
      pkf.ucomp_size != file.cd_entry.ucomp_size ||
      pkf.comp_method != file.cd_entry.comp_method ||
      file.filename != std::string_view((const char*)lfile, pkf.fname_len)) {
    std::cerr << "Directory entry doesn't match local file header, Bailing "
              << pkf.fname_len << " " << pkf.comp_size << " " << pkf.ucomp_size
              << " " << pkf.comp_method << " " << file.cd_entry.fname_len << " "
              << file.cd_entry.comp_size << " " << file.cd_entry.ucomp_size
              << " " << file.cd_entry.comp_method << " extra " << pkf.extra_len
              << "\n";
    return false;
  }

  lfile += pkf.fname_len;
  lfile += pkf.extra_len;

  uLongf dlen = bufsize;
  int zlibrv = jar_uncompress(outbuffer, &dlen, lfile, pkf.comp_size,
                              file.cd_entry.comp_method);
  if (zlibrv != Z_OK) {
    std::cerr << "uncompress failed with code " << zlibrv << ", Bailing\n";
    return false;
  }
  if (dlen != pkf.ucomp_size) {
    std::cerr << "mis-match on uncompressed size, Bailing\n";
    return false;
  }
  return true;
}

constexpr size_t kStartBufferSize = 128 * 1024;

bool process_jar_entries(
    const DexLocation* location,
    std::vector<jar_entry>& files,
    const uint8_t* mapping,
    const size_t map_size,
    Scope* classes,
    const attribute_hook_t& attr_hook,
    const jar_loader::duplicate_allowed_hook_t& is_allowed) {
  ssize_t bufsize = kStartBufferSize;
  std::unique_ptr<uint8_t[]> outbuffer = std::make_unique<uint8_t[]>(bufsize);
  constexpr std::string_view kClassEndString = ".class";
  init_basic_types();
  for (auto& file : files) {
    if (file.cd_entry.ucomp_size == 0) continue;

    // Skip non-class files
    std::string_view filename = file.filename;
    if (filename.length() < kClassEndString.length()) continue;
    auto endcomp =
        filename.substr(filename.length() - kClassEndString.length());
    if (endcomp != kClassEndString) continue;

    // Resize output if necessary.
    if (bufsize < file.cd_entry.ucomp_size) {
      while (bufsize < file.cd_entry.ucomp_size)
        bufsize *= 2;
      outbuffer = std::make_unique<uint8_t[]>(bufsize);
    }

    if (!decompress_class(file, mapping, map_size, outbuffer.get(), bufsize)) {
      return false;
    }

    if (!parse_class(outbuffer.get(), bufsize, classes, attr_hook, is_allowed,
                     location)) {
      return false;
    }
  }
  return true;
}

} // namespace

namespace jar_loader {

bool default_duplicate_allow_fn(const DexClass* c, const std::string&) {
  return !boost::starts_with(c->str(), "Landroid");
}

} // namespace jar_loader

bool process_jar(const DexLocation* location,
                 const uint8_t* mapping,
                 size_t size,
                 Scope* classes,
                 const attribute_hook_t& attr_hook,
                 const jar_loader::duplicate_allowed_hook_t& is_allowed) {
  pk_cdir_end pce;
  std::vector<jar_entry> files;
  if (!find_central_directory(mapping, size, pce)) {
    return false;
  }
  if (!validate_pce(pce, size)) {
    return false;
  }
  if (!get_jar_entries(mapping, size, pce, files)) {
    return false;
  }
  if (!process_jar_entries(location, files, mapping, size, classes, attr_hook,
                           is_allowed)) {
    return false;
  }
  return true;
}

bool load_jar_file(const DexLocation* location,
                   Scope* classes,
                   const attribute_hook_t& attr_hook,
                   const jar_loader::duplicate_allowed_hook_t& is_allowed) {
  boost::iostreams::mapped_file file;
  try {
    file.open(location->get_file_name().c_str(),
              boost::iostreams::mapped_file::readonly);
  } catch (const std::exception& e) {
    std::cerr << "error: cannot open jar file: " << location->get_file_name()
              << "\n";
    return false;
  }

  auto mapping = reinterpret_cast<const uint8_t*>(file.const_data());
  if (!process_jar(location, mapping, file.size(), classes, attr_hook,
                   is_allowed)) {
    std::cerr << "error: cannot process jar: " << location->get_file_name()
              << "\n";
    return false;
  }
  return true;
}

//#define LOCAL_MAIN
#ifdef LOCAL_MAIN
int main(int argc, char* argv[]) {
  if (argc < 2) {
    fprintf(stderr, "You must specify a jar file\n");
    return -1;
  }
  for (int jarno = 1; jarno < argc; jarno++) {
    if (!load_jar_file(DexLocation::make_location("", argv[jarno]))) {
      fprintf(stderr, "Failed to load jar %s, bailing\n", argv[jarno]);
      return -2;
    }
  }
}
#endif
