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
#include <utility>
#include <vector>
#include <zlib.h>

#if IS_WINDOWS
#include <Winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <netinet/in.h>
#endif

#include "Creators.h"
#include "DeterministicContainers.h"
#include "DexClass.h"
#include "DuplicateClasses.h"
#include "JarLoader.h"
#include "Show.h"
#include "Trace.h"
#include "TypeUtil.h"
#include "Util.h"

/******************
 * Begin Class Loading code.
 */

namespace JarLoaderUtil {
uint32_t read32(uint8_t*& buffer, uint8_t* buffer_end) {
  uint32_t rv;
  auto* next = buffer + sizeof(uint32_t);
  always_assert_type_log(next <= buffer_end, BUFFER_END_EXCEEDED,
                         "Buffer overflow");
  memcpy(&rv, buffer, sizeof(uint32_t));
  buffer = next;
  return htonl(rv);
}

uint16_t read16(uint8_t*& buffer, uint8_t* buffer_end) {
  uint16_t rv;
  auto* next = buffer + sizeof(uint16_t);
  always_assert_type_log(next <= buffer_end, BUFFER_END_EXCEEDED,
                         "Buffer overflow");
  memcpy(&rv, buffer, sizeof(uint16_t));
  buffer = next;
  return htons(rv);
}

uint8_t read8(uint8_t*& buffer, uint8_t* buffer_end) {
  always_assert_type_log(buffer < buffer_end, BUFFER_END_EXCEEDED,
                         "Buffer overflow");
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

cp_entry parse_cp_entry(uint8_t*& buffer, uint8_t* buffer_end) {
  cp_entry cpe;
  cpe.tag = read8(buffer, buffer_end);
  switch (cpe.tag) {
  case CP_CONST_CLASS:
  case CP_CONST_STRING:
  case CP_CONST_METHTYPE:
  case CP_CONST_MODULE:
  case CP_CONST_PACKAGE:
    cpe.s0 = read16(buffer, buffer_end);
    return cpe;
  case CP_CONST_FIELD:
  case CP_CONST_METHOD:
  case CP_CONST_INTERFACE:
  case CP_CONST_NAMEANDTYPE:
    cpe.s0 = read16(buffer, buffer_end);
    cpe.s1 = read16(buffer, buffer_end);
    return cpe;
  case CP_CONST_METHHANDLE:
    cpe.s0 = read8(buffer, buffer_end);
    cpe.s1 = read16(buffer, buffer_end);
    return cpe;
  case CP_CONST_INT:
  case CP_CONST_FLOAT:
    cpe.i0 = read32(buffer, buffer_end);
    return cpe;
  case CP_CONST_LONG:
  case CP_CONST_DOUBLE:
    cpe.i0 = read32(buffer, buffer_end);
    cpe.i1 = read32(buffer, buffer_end);
    return cpe;
  case CP_CONST_UTF8:
    cpe.len = read16(buffer, buffer_end);
    cpe.data = buffer;
    buffer += cpe.len;
    always_assert_type_log(buffer <= buffer_end, BUFFER_END_EXCEEDED,
                           "Buffer overflow");
    return cpe;
  case CP_CONST_INVOKEDYN:
    always_assert_type_log(cpe.tag != CP_CONST_INVOKEDYN,
                           RedexError::INVALID_JAVA,
                           "INVOKEDYN constant unsupported");
    UNREACHABLE();
  default:
    always_assert_type_log(false, RedexError::INVALID_JAVA,
                           "Unrecognized constant pool tag 0x%x", cpe.tag);
    UNREACHABLE();
  }
}

void skip_attributes(uint8_t*& buffer, uint8_t* buffer_end) {
  /*
   * TODO:
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

constexpr size_t MAX_CLASS_NAMELEN = static_cast<size_t>(8 * 1024);

DexType* make_dextype_from_cref(std::vector<cp_entry>& cpool, uint16_t cref) {
  char nbuffer[MAX_CLASS_NAMELEN];
  always_assert_type_log(cref < cpool.size(), RedexError::INVALID_JAVA,
                         "Illegal cref");
  always_assert_type_log(cpool[cref].tag == CP_CONST_CLASS,
                         RedexError::INVALID_JAVA,
                         "Non-class ref in get_class_name");
  uint16_t utf8ref = cpool[cref].s0;
  always_assert_type_log(utf8ref < cpool.size(), RedexError::INVALID_JAVA,
                         "utf8 ref out of bound");
  const cp_entry& utf8cpe = cpool[utf8ref];
  always_assert_type_log(utf8cpe.tag == CP_CONST_UTF8, RedexError::INVALID_JAVA,
                         "Non-utf8 ref in get_utf8");
  always_assert_type_log(utf8cpe.len <= (MAX_CLASS_NAMELEN + 3),
                         RedexError::INVALID_JAVA,
                         "classname is greater than max");
  try {
    nbuffer[0] = 'L';
    memcpy(nbuffer + 1, utf8cpe.data, utf8cpe.len);
    nbuffer[1 + utf8cpe.len] = ';';
    nbuffer[2 + utf8cpe.len] = '\0';
    return DexType::make_type(nbuffer);
  } catch (std::invalid_argument&) {
    return nullptr;
  }
}

std::string_view extract_utf8(std::vector<cp_entry>& cpool, uint16_t utf8ref) {
  always_assert_type_log(utf8ref < cpool.size(), RedexError::INVALID_JAVA,
                         "utf8 ref out of bound");
  const cp_entry& utf8cpe = cpool[utf8ref];
  always_assert_type_log(utf8cpe.tag == CP_CONST_UTF8, RedexError::INVALID_JAVA,
                         "Non-utf8 ref in get_utf8");
  always_assert_type_log(
      utf8cpe.len <= (MAX_CLASS_NAMELEN - 1), RedexError::INVALID_JAVA,
      "Name is greater (%u) than max (%zu)", utf8cpe.len, MAX_CLASS_NAMELEN);
  return std::string_view((const char*)utf8cpe.data, utf8cpe.len);
}

DexField* make_dexfield(std::vector<cp_entry>& cpool,
                        DexType* self,
                        cp_field_info& finfo,
                        UnorderedSet<const DexField*>& added) {
  std::string_view nbuffer = extract_utf8(cpool, finfo.nameNdx);
  std::string_view dbuffer = extract_utf8(cpool, finfo.descNdx);
  always_assert_type_log(!nbuffer.empty(), INVALID_JAVA, "Empty field name");
  const auto* name = DexString::make_string(nbuffer);
  DexType* desc = DexType::make_type(dbuffer);
  DexField* field =
      static_cast<DexField*>(DexField::make_field(self, name, desc));

  // We cannot do an existence check because of mixed sources. At least make
  // sure we only add a field here once.
  auto inserted = added.insert(field).second;
  always_assert_type_log(inserted, INVALID_JAVA, "Duplicate field %s",
                         SHOW(field));

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
  always_assert_type_log(!buf.empty(), RedexError::INVALID_JAVA,
                         "Invalid empty parse-type");

  char desc = buf.at(0);
  const std::string_view buf_start = buf;
  buf = buf.substr(1); // Simplifies primitive types.
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
  default:
    break; // Fall through to main switch below
  }

  buf = buf_start;
  const size_t start_size = buf.size();
  switch (desc) {
  case 'L': {
    // Find semicolon.
    for (size_t i = 1; i < buf.size(); i++) {
      if (buf[i] == ';') {
        always_assert_type_log(i != 1, RedexError::INVALID_JAVA,
                               "Empty class name");
        auto ret = buf.substr(0, i + 1);
        buf = buf.substr(i + 1);
        redex_assert(buf.size() < start_size);
        return DexType::make_type(ret);
      }
      // TODO: Check valid class name chars.
    }
    always_assert_type_log(
        false, RedexError::INVALID_JAVA,
        "Could not parse reference type, no suffix semicolon");
  }
  case '[': {
    // Figure out array depth.
    auto depth = [&]() {
      for (size_t i = 1; i < buf.size(); ++i) {
        if (buf[i] != '[') {
          return i;
        }
      }
      return buf.size();
    }();
    always_assert_type_log(depth != buf.size(), RedexError::INVALID_JAVA,
                           "Could not parse array type, no element type");

    // Easiest to go recursive here.
    buf = buf.substr(depth);
    auto* elem_type = parse_type(buf);
    redex_assert(elem_type != nullptr);
    redex_assert(!type::is_array(elem_type));
    redex_assert(buf.size() < start_size);
    auto* ret = type::make_array_type(elem_type, depth);

    return ret;
  }
  default:
    always_assert_type_log(false, RedexError::INVALID_JAVA,
                           "Invalid parse-type '%c'", desc);
    UNREACHABLE();
  }
}

DexTypeList* extract_arguments(std::string_view& buf) {
  always_assert_type_log(buf.size() >= 2, RedexError::INVALID_JAVA,
                         "Invalid argument list without open-close-parens");

  buf = buf.substr(1);
  if (buf.at(0) == ')') {
    buf = buf.substr(1);
    return DexTypeList::make_type_list({});
  }
  DexTypeList::ContainerType args;
  while (buf.at(0) != ')') {
    DexType* dtype = parse_type(buf);
    redex_assert(dtype != nullptr);
    always_assert_type_log(dtype != sSimpleTypeV, RedexError::INVALID_JAVA,
                           "Invalid argument type 'V' in args");
    args.push_back(dtype);
    always_assert_type_log(!buf.empty(), RedexError::INVALID_JAVA,
                           "Missing close parens");
  }
  buf = buf.substr(1);
  return DexTypeList::make_type_list(std::move(args));
}

DexMethod* make_dexmethod(std::vector<cp_entry>& cpool,
                          DexType* self,
                          cp_method_info& finfo,
                          UnorderedSet<const DexMethod*>& added) {
  std::string_view nbuffer = extract_utf8(cpool, finfo.nameNdx);
  std::string_view dbuffer = extract_utf8(cpool, finfo.descNdx);
  always_assert_type_log(!nbuffer.empty(), INVALID_JAVA, "Empty method name");
  const auto* name = DexString::make_string(nbuffer);
  std::string_view ptr = dbuffer;
  DexTypeList* tlist = extract_arguments(ptr);
  redex_assert(tlist != nullptr);
  DexType* rtype = parse_type(ptr);
  redex_assert(rtype != nullptr);
  DexProto* proto = DexProto::make_proto(rtype, tlist);
  DexMethod* method =
      static_cast<DexMethod*>(DexMethod::make_method(self, name, proto));
  auto inserted = added.insert(method).second;
  always_assert_type_log(inserted, INVALID_JAVA, "Duplicate method %s",
                         SHOW(method));
  always_assert_type_log(!method->is_concrete(), RedexError::INVALID_JAVA,
                         "Pre-concrete method attempted to load '%s'",
                         SHOW(method));
  uint32_t access = finfo.aflags;
  bool is_virt = true;
  if (nbuffer[0] == '<') {
    is_virt = false;
    if (nbuffer[1] == 'i') {
      access |= ACC_CONSTRUCTOR;
    }
  } else if ((access & (ACC_PRIVATE | ACC_STATIC)) != 0u) {
    is_virt = false;
  }
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
  auto* buffer_end = buffer + buffer_size;
  uint32_t magic = read32(buffer, buffer_end);
  uint16_t vminor DEBUG_ONLY = read16(buffer, buffer_end);
  uint16_t vmajor DEBUG_ONLY = read16(buffer, buffer_end);
  uint16_t cp_count = read16(buffer, buffer_end);
  always_assert_type_log(magic == kClassMagic, RedexError::INVALID_JAVA,
                         "Bad class magic 0x%x", magic);
  std::vector<cp_entry> cpool;
  cpool.resize(cp_count);
  /* The zero'th entry is always empty.  Java is annoying. */
  for (size_t i = 1; i < cp_count; i++) {
    cpool[i] = parse_cp_entry(buffer, buffer_end);
    if (cpool[i].tag == CP_CONST_LONG || cpool[i].tag == CP_CONST_DOUBLE) {
      always_assert_type_log(i + 1 < cp_count, RedexError::INVALID_JAVA,
                             "Bad long/double constant");
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
  always_assert_type_log(self != nullptr, RedexError::INVALID_JAVA,
                         "Bad class cpool index %u", clazz);
  DexClass* cls = type_class(self);
  if (cls != nullptr) {
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

      assert_or_throw(is_allowed(cls, jar_location->get_file_name()),
                      RedexError::DUPLICATE_CLASSES,
                      "Found duplicate class in two different files.",
                      {{"class", SHOW(self)},
                       {"jar", jar_location->get_file_name()},
                       {"dex", cls->get_location()->get_file_name()}});
    }
    return true;
  }

  ClassCreator cc(self, jar_location);
  cc.set_external();
  if (super != 0) {
    DexType* sclazz = make_dextype_from_cref(cpool, super);
    always_assert_type_log(sclazz != nullptr, RedexError::INVALID_JAVA,
                           "Bad super class cpool index %u", super);
    cc.set_super(sclazz);
  } else {
    always_assert_type_log(self->get_name()->str() == "Ljava/lang/Object;",
                           RedexError::INVALID_JAVA,
                           "Missing super for class cpool index %u", clazz);
  }
  cc.set_access((DexAccessFlags)aflags);

  for (int i = 0; i < ifcount; i++) {
    uint16_t iface = read16(buffer, buffer_end);
    DexType* iftype = make_dextype_from_cref(cpool, iface);
    always_assert_type_log(iftype != nullptr, RedexError::INVALID_JAVA,
                           "Bad interface cpool index %u", super);
    cc.add_interface(iftype);
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
          std::string_view attribute_name =
              extract_utf8(cpool, attribute_name_index);
          attr_hook(field_or_method, attribute_name, attrPtr, buffer_end);
          attrPtr += attribute_length;
        }
      };

  UnorderedSet<const DexField*> added_fields;
  for (int i = 0; i < fcount; i++) {
    cp_field_info cpfield;
    cpfield.aflags = read16(buffer, buffer_end);
    cpfield.nameNdx = read16(buffer, buffer_end);
    cpfield.descNdx = read16(buffer, buffer_end);
    uint8_t* attrPtr = buffer;
    skip_attributes(buffer, buffer_end);
    DexField* field = make_dexfield(cpool, self, cpfield, added_fields);
    redex_assert(field != nullptr);
    cc.add_field(field);
    invoke_attr_hook({field}, attrPtr);
  }

  uint16_t mcount = read16(buffer, buffer_end);
  UnorderedSet<const DexMethod*> added_methods;
  if (mcount != 0u) {
    for (int i = 0; i < mcount; i++) {
      cp_method_info cpmethod;
      cpmethod.aflags = read16(buffer, buffer_end);
      cpmethod.nameNdx = read16(buffer, buffer_end);
      cpmethod.descNdx = read16(buffer, buffer_end);

      uint8_t* attrPtr = buffer;
      skip_attributes(buffer, buffer_end);
      DexMethod* method = make_dexmethod(cpool, self, cpmethod, added_methods);
      redex_assert(method != nullptr);
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
  auto* buf = ifs.rdbuf();
  size_t size = buf->pubseekoff(0, ifs.end, ifs.in);
  buf->pubseekpos(0, ifs.in);
  auto buffer = std::make_unique<char[]>(size);
  buf->sgetn(buffer.get(), static_cast<std::streamsize>(size));
  const auto* jar_location = DexLocation::make_location("", filename);
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

pk_cdir_end find_central_directory(const uint8_t* mapping, ssize_t size) {
  ssize_t soffset = size - static_cast<ssize_t>(sizeof(pk_cdir_end));
  always_assert_type_log(soffset >= 0, RedexError::INVALID_JAVA,
                         "Zip too small");
  ssize_t eoffset = std::max((ssize_t)0, soffset - kMaxCDirEndSearch);
  do {
    const uint8_t* cdsearch = mapping + soffset;
    if (memcmp(cdsearch, kCDirEnd.data(), kCDirEnd.size()) == 0) {
      pk_cdir_end pce;
      memcpy(&pce, cdsearch, sizeof(pk_cdir_end));
      return pce;
    }
  } while (soffset-- > eoffset);
  always_assert_type_log(false, RedexError::INVALID_JAVA,
                         "End of central directory record not found");
  UNREACHABLE();
}

void validate_pce(pk_cdir_end& pce, ssize_t size) {
  /* We only support a limited feature set.  We
   * don't support disk-spanning, so bail if that's the case.
   */
  always_assert_type_log(pce.cd_diskno == pce.diskno && pce.cd_diskno == 0 &&
                             pce.cd_entries == pce.cd_disk_entries,
                         RedexError::INVALID_JAVA,
                         "Disk spanning is not supported");
  ssize_t data_size = size - static_cast<ssize_t>(sizeof(pk_cdir_end));
  always_assert_type_log(pce.cd_disk_offset + pce.cd_size <= data_size,
                         RedexError::INVALID_JAVA,
                         "Central directory overflow, invalid pce structure");
}

jar_entry extract_jar_entry(const uint8_t*& mapping,
                            size_t& offset,
                            size_t total_size) {
  jar_entry je;
  always_assert_type_log(offset + kCDFile.size() <= total_size,
                         RedexError::INVALID_JAVA,
                         "Reading mapping out of bound");
  always_assert_type_log(memcmp(mapping, kCDFile.data(), kCDFile.size()) == 0,
                         RedexError::INVALID_JAVA,
                         "Invalid central directory entry");
  offset += sizeof(pk_cd_file);
  always_assert_type_log(offset < total_size, RedexError::INVALID_JAVA,
                         "Reading mapping out of bound");
  memcpy(&je.cd_entry, mapping, sizeof(pk_cd_file));
  mapping += sizeof(pk_cd_file);
  offset += je.cd_entry.fname_len;
  always_assert_type_log(offset < total_size, RedexError::INVALID_JAVA,
                         "Reading mapping out of bound");
  je.filename = std::string((const char*)mapping, je.cd_entry.fname_len);
  mapping += je.cd_entry.fname_len;
  offset = offset + je.cd_entry.extra_len + je.cd_entry.comment_len;
  mapping += je.cd_entry.extra_len;
  mapping += je.cd_entry.comment_len;
  return je;
}

std::vector<jar_entry> get_jar_entries(const uint8_t* mapping,
                                       size_t size,
                                       pk_cdir_end& pce) {
  const uint8_t* cdir = mapping + pce.cd_disk_offset;
  std::vector<jar_entry> files;
  files.reserve(pce.cd_entries);
  size_t offset = pce.cd_disk_offset;
  for (int entry = 0; entry < pce.cd_entries; entry++) {
    files.emplace_back(extract_jar_entry(cdir, offset, size));
  }
  return files;
}

void jar_uncompress(Bytef* dest,
                    uLongf* destLen,
                    const Bytef* source,
                    uLong sourceLen,
                    uint32_t comp_method) {
  if (comp_method == kCompMethodStore) {
    always_assert_type_log(sourceLen <= *destLen, RedexError::INVALID_JAVA,
                           "Not enough space for STOREd entry: %lu vs %lu",
                           sourceLen, *destLen);
    memcpy(dest, source, sourceLen);
    *destLen = sourceLen;
    return;
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
  always_assert_type_log(err == Z_OK, RedexError::INVALID_JAVA,
                         "Failed decompression");

  err = inflate(&stream, Z_FINISH);
  if (err != Z_STREAM_END) {
    inflateEnd(&stream);
    always_assert_type_log(err == Z_STREAM_END, RedexError::INVALID_JAVA,
                           "Failed decompression");
  }
  *destLen = stream.total_out;

  err = inflateEnd(&stream);
  always_assert_type_log(err == Z_OK, RedexError::INVALID_JAVA,
                         "Failed inflateEnd");
}

void decompress_class(jar_entry& file,
                      const uint8_t* mapping,
                      size_t map_size,
                      uint8_t* outbuffer,
                      ssize_t bufsize) {
  always_assert_type_log(file.cd_entry.comp_method == kCompMethodDeflate ||
                             file.cd_entry.comp_method == kCompMethodStore,
                         RedexError::INVALID_JAVA,
                         "Unknown compression method %u for %s",
                         file.cd_entry.comp_method, file.filename.c_str());

  static_assert(kLFile.size() <= sizeof(pk_lfile));
  always_assert_type_log(file.cd_entry.disk_offset + sizeof(pk_lfile) <
                             map_size,
                         RedexError::INVALID_JAVA,
                         "Entry out of map bounds!");
  const uint8_t* lfile = mapping + file.cd_entry.disk_offset;
  always_assert_type_log(memcmp(lfile, kLFile.data(), kLFile.size()) == 0,
                         RedexError::INVALID_JAVA,
                         "Invalid local file entry");

  pk_lfile pkf;
  memcpy(&pkf, lfile, sizeof(pk_lfile));
  if (pkf.comp_size == 0 && pkf.ucomp_size == 0 &&
      pkf.comp_size != file.cd_entry.comp_size &&
      pkf.ucomp_size != file.cd_entry.ucomp_size) {
    pkf.comp_size = file.cd_entry.comp_size;
    pkf.ucomp_size = file.cd_entry.ucomp_size;
  }

  lfile += sizeof(pk_lfile);

  always_assert_type_log(file.cd_entry.disk_offset + sizeof(pk_lfile) +
                                 pkf.fname_len + pkf.extra_len + pkf.comp_size <
                             map_size,
                         RedexError::INVALID_JAVA,
                         "Complete entry exceeds mapping bounds.");

  always_assert_type_log(
      pkf.fname_len == file.cd_entry.fname_len &&
          pkf.comp_size == file.cd_entry.comp_size &&
          pkf.ucomp_size == file.cd_entry.ucomp_size &&
          pkf.comp_method == file.cd_entry.comp_method &&
          file.filename == std::string_view((const char*)lfile, pkf.fname_len),
      RedexError::INVALID_JAVA,
      "Directory entry doesn't match local file header %u %u %u %u %u "
      "%u %u %u extra %u",
      pkf.fname_len, pkf.comp_size, pkf.ucomp_size, pkf.comp_method,
      file.cd_entry.fname_len, file.cd_entry.comp_size,
      file.cd_entry.ucomp_size, file.cd_entry.comp_method, pkf.extra_len);

  lfile += pkf.fname_len;
  lfile += pkf.extra_len;

  uLongf dlen = bufsize;
  jar_uncompress(outbuffer, &dlen, lfile, pkf.comp_size,
                 file.cd_entry.comp_method);
  always_assert_type_log(dlen == pkf.ucomp_size, RedexError::INVALID_JAVA,
                         "mis-match on uncompressed size");
}

constexpr size_t kStartBufferSize = static_cast<size_t>(128 * 1024);
constexpr size_t kMaxBufferSize = static_cast<size_t>(8 * 1024 * 1024);

template <typename Fn, typename InitFn>
void process_jar_entries(std::vector<jar_entry>& files,
                         const uint8_t* mapping,
                         const size_t map_size,
                         const Fn& fn,
                         const InitFn& init_fn) {
  ssize_t bufsize = kStartBufferSize;
  std::unique_ptr<uint8_t[]> outbuffer = std::make_unique<uint8_t[]>(bufsize);
  constexpr std::string_view kClassEndString = ".class";
  init_fn();
  for (auto& file : files) {
    if (file.cd_entry.ucomp_size == 0) {
      continue;
    }

    // Skip non-class files
    std::string_view filename = file.filename;
    if (filename.length() < kClassEndString.length()) {
      continue;
    }
    auto endcomp =
        filename.substr(filename.length() - kClassEndString.length());
    if (endcomp != kClassEndString) {
      continue;
    }

    // Reject uncharacteristically large files.
    always_assert_type_log(file.cd_entry.ucomp_size <= kMaxBufferSize,
                           INVALID_JAVA, "Entry %s with size %u is too large",
                           file.filename.c_str(), file.cd_entry.ucomp_size);

    // Resize output if necessary.
    if (bufsize < file.cd_entry.ucomp_size) {
      while (bufsize < file.cd_entry.ucomp_size) {
        bufsize *= 2;
      }
      outbuffer = std::make_unique<uint8_t[]>(bufsize);
    }

    decompress_class(file, mapping, map_size, outbuffer.get(), bufsize);

    fn(outbuffer.get(), bufsize);
  }
}

template <typename Fn, typename InitFn>
void process_jar_impl(const uint8_t* mapping,
                      size_t size,
                      const Fn& fn,
                      const InitFn& init_fn) {
  auto pce = find_central_directory(mapping, static_cast<ssize_t>(size));
  validate_pce(pce, static_cast<ssize_t>(size));
  auto files = get_jar_entries(mapping, size, pce);
  process_jar_entries(files, mapping, size, fn, init_fn);
}

} // namespace

namespace jar_loader {

bool default_duplicate_allow_fn(const DexClass* c, const std::string&) {
  return !boost::starts_with(c->str(), "Landroid");
}

} // namespace jar_loader

void process_jar(const uint8_t* mapping,
                 size_t size,
                 const std::function<void(uint8_t*, size_t)>& on_class) {
  process_jar_impl(mapping, size, on_class, []() {});
}

bool load_jar_file(const DexLocation* location,
                   Scope* classes,
                   const attribute_hook_t& attr_hook,
                   const jar_loader::duplicate_allowed_hook_t& is_allowed) {
  boost::iostreams::mapped_file file;
  file.open(location->get_file_name().c_str(),
            boost::iostreams::mapped_file::readonly);

  const auto* mapping = reinterpret_cast<const uint8_t*>(file.const_data());
  auto init_fn = []() { init_basic_types(); };
  auto on_class = [classes, &attr_hook, &is_allowed, location](uint8_t* buffer,
                                                               size_t size) {
    auto parse_result =
        parse_class(buffer, size, classes, attr_hook, is_allowed, location);
    always_assert_type_log(parse_result, RedexError::INVALID_JAVA,
                           "Failed to parse class");
  };
  process_jar_impl(mapping, file.size(), on_class, init_fn);
  return true;
}

// #define LOCAL_MAIN
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
