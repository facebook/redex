/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Code for parsing and building OAT files for multiple android versions. See
// OatFile::build and OatFile::parse, below.

#if __ANDROID__
#include <museum/5.0.0/bionic/libc/android/legacy_stdlib_inlines.h>
#include <museum/5.0.0/bionic/libc/ctype.h>
#include <museum/5.0.0/bionic/libc/errno.h>
#include <museum/5.0.0/bionic/libc/locale.h>
#include <museum/5.0.0/bionic/libc/math.h>
#include <museum/5.0.0/bionic/libc/pthread.h>
#include <museum/5.0.0/bionic/libc/stdlib.h>
#include <museum/5.0.0/bionic/libc/sys/stat.h>
#include <museum/5.0.0/bionic/libc/wchar.h>
#include <museum/5.0.0/bionic/libc/wctype.h>
#include <museum/5.0.0/external/libcxx/support/android/locale_bionic.h>
#endif // __ANDROID__

#include "OatmealUtil.h"
#include "QuickData.h"
#include "Util.h"
#include "dex.h"
#include "dump-oat.h"
#include "elf-writer.h"
#include "memory-accounter.h"
#include "vdex.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#define PACK __attribute__((packed))

#define READ_WORD(dest, ptr)                 \
  do {                                       \
    cur_ma()->memcpyAndMark((dest), ptr, 4); \
    ptr += 4;                                \
  } while (false);

namespace {

// "86827de6f1ef3407f8dc98b76382d3a6e0759ab3" is the SHA1 digest for
// 'created_by_oatmeal'.
const char* kCreatedByOatmeal = "86827de6f1ef3407f8dc98b76382d3a6e0759ab3";

VdexVersion vdexVersion(OatVersion oat_version) {
  switch (oat_version) {
  case OatVersion::V_124:
    return VdexVersion::V_006;
  case OatVersion::V_131:
    return VdexVersion::V_010;
  default:
    return VdexVersion::UNKNOWN;
  }
}

OatVersion versionInt(const std::string& version_str) {
  if (version_str == "039") {
    return OatVersion::V_039;
  } else if (version_str == "045") {
    return OatVersion::V_045;
  } else if (version_str == "064") {
    return OatVersion::V_064;
  } else if (version_str == "067") {
    return OatVersion::V_067;
  } else if (version_str == "079") {
    return OatVersion::V_079;
  } else if (version_str == "088") {
    return OatVersion::V_088;
  } else if (version_str == "124") {
    return OatVersion::V_124;
  } else if (version_str == "131") {
    return OatVersion::V_131;
  } else {
    CHECK(false, "Bad version %s", version_str.c_str());
  }
  return OatVersion::UNKNOWN;
}

struct PACK ImageInfo_064 {
  int32_t patch_delta = 0;
  uint32_t oat_checksum = 0;
  uint32_t data_begin = 0;

  ImageInfo_064() = default;
  ImageInfo_064(int32_t pd, uint32_t oc, uint32_t db)
      : patch_delta(pd), oat_checksum(oc), data_begin(db) {}
};

struct PACK ArtImageHeader {
  enum class Version {
    V_009 = 0x00393030,
    V_012 = 0x00323130,
    V_017 = 0x00373130,
  };

  uint32_t magic;
  uint32_t version;

  uint32_t image_begin;
  uint32_t image_size;

  // next two fields present in versions 012
  // not present in versions 017
  uint32_t image_bitmap_offset;
  uint32_t image_bitmap_size;

  uint32_t oat_checksum;
  uint32_t oat_file_begin;
  uint32_t oat_data_begin;
  uint32_t oat_data_end;
  uint32_t oat_file_end;
  int32_t patch_delta;
  uint32_t image_roots;
  uint32_t pointer_size;
  uint32_t compile_pic;

  static std::unique_ptr<ArtImageHeader> parse(FileHandle& fh) {
    constexpr auto size = sizeof(ArtImageHeader);
    auto buf = std::make_unique<char[]>(size);
    auto ptr = buf.get();
    auto num_read = fh.fread(ptr, size, 1);
    if (num_read != 1) {
      return nullptr;
    }

    auto ret = std::make_unique<ArtImageHeader>();
    READ_WORD(&ret->magic, ptr);
    READ_WORD(&ret->version, ptr);

    READ_WORD(&ret->image_begin, ptr);
    READ_WORD(&ret->image_size, ptr);

    switch (static_cast<Version>(ret->version)) {
    case Version::V_009:
    case Version::V_012:
      READ_WORD(&ret->image_bitmap_offset, ptr);
      READ_WORD(&ret->image_bitmap_size, ptr);
      break;

    case Version::V_017:
      break;
    }

    READ_WORD(&ret->oat_checksum, ptr);

    READ_WORD(&ret->oat_file_begin, ptr);
    READ_WORD(&ret->oat_data_begin, ptr);
    READ_WORD(&ret->oat_data_end, ptr);
    READ_WORD(&ret->oat_file_end, ptr);
    READ_WORD(&ret->patch_delta, ptr);
    READ_WORD(&ret->image_roots, ptr);
    READ_WORD(&ret->pointer_size, ptr);
    READ_WORD(&ret->compile_pic, ptr);

    return ret;
  }
};

struct PACK OatHeader_Common {
  uint32_t magic = 0;
  uint32_t version = 0;

  uint32_t adler32_checksum = 0;

  static OatHeader_Common parse(ConstBuffer buf) {
    OatHeader_Common header;
    CHECK(buf.len >= sizeof(OatHeader_Common));
    cur_ma()->memcpyAndMark(&header, buf.ptr, sizeof(OatHeader_Common));
    return header;
  }

  void print() {
    // char magic_str[5] = {};
    // char version_str[5] = {};
    // memcpy(magic_str, &magic, 3); // magic has a newline character at idx 4.
    // memcpy(version_str, &version, 4);
    printf(
        "OatHeader_Common: {magic: 0x%08x, \
      version: 0x%08x, \
      checksum: 0x%08x}\n",
        magic,
        version,
        adler32_checksum);
  }
};

struct PACK OatHeader {
  OatHeader_Common common;

  InstructionSet instruction_set = InstructionSet::kNone;
  uint32_t instruction_set_features_bitmap = 0;
  uint32_t dex_file_count = 0;
  uint32_t oat_dex_files_offset = 0; // Only on API 27
  uint32_t executable_offset = 0;
  uint32_t interpreter_to_interpreter_bridge_offset = 0;
  uint32_t interpreter_to_compiled_code_bridge_offset = 0;
  uint32_t jni_dlsym_lookup_offset = 0;

  // These three fields are not present in version 064 and up.
  uint32_t portable_imt_conflict_trampoline_offset = 0;
  uint32_t portable_resolution_trampoline_offset = 0;
  uint32_t portable_to_interpreter_bridge_offset = 0;

  uint32_t quick_generic_jni_trampoline_offset = 0;
  uint32_t quick_imt_conflict_trampoline_offset = 0;
  uint32_t quick_resolution_trampoline_offset = 0;
  uint32_t quick_to_interpreter_bridge_offset = 0;

  int32_t image_patch_delta = 0;

  uint32_t image_file_location_oat_checksum = 0;
  uint32_t image_file_location_oat_data_begin = 0;

  uint32_t key_value_store_size = 0;

  // note: variable width data at end
  uint8_t key_value_store[0];

  static size_t size(OatVersion version) {
    if (version == OatVersion::V_039 || version == OatVersion::V_045) {
      // Subtract the size of oat_dex_files_offset which is only present
      // in API 27.
      return sizeof(OatHeader) - 1 * sizeof(uint32_t);
    } else if (version == OatVersion::V_131) {
      // Minus the 3 fields that are not present in 064 and above.
      return sizeof(OatHeader) - 3 * sizeof(uint32_t);
    } else {
      // Minus the 3 fields not in 064 and oat_dex_files_offset which only
      // shows up in 131.
      return sizeof(OatHeader) - 4 * sizeof(uint32_t);
    }
  }

  size_t size() const {
    return OatHeader::size(static_cast<OatVersion>(common.version));
  }

  static OatHeader parse(ConstBuffer buf) {
    OatHeader header;
    CHECK(buf.len >= sizeof(OatHeader_Common));

    cur_ma()->memcpyAndMark(&header, buf.ptr, sizeof(OatHeader_Common));
    auto ptr = buf.ptr + sizeof(OatHeader_Common);

    READ_WORD(&header.instruction_set, ptr);
    READ_WORD(&header.instruction_set_features_bitmap, ptr);
    READ_WORD(&header.dex_file_count, ptr);

    if (header.common.version == static_cast<uint32_t>(OatVersion::V_131)) {
      READ_WORD(&header.oat_dex_files_offset, ptr);
    }

    READ_WORD(&header.executable_offset, ptr);
    READ_WORD(&header.interpreter_to_interpreter_bridge_offset, ptr);
    READ_WORD(&header.interpreter_to_compiled_code_bridge_offset, ptr);
    READ_WORD(&header.jni_dlsym_lookup_offset, ptr);

    // These three fields are not present in version 064 and up.
    if (header.common.version == static_cast<uint32_t>(OatVersion::V_045) ||
        header.common.version == static_cast<uint32_t>(OatVersion::V_039)) {
      READ_WORD(&header.portable_imt_conflict_trampoline_offset, ptr);
      READ_WORD(&header.portable_resolution_trampoline_offset, ptr);
      READ_WORD(&header.portable_to_interpreter_bridge_offset, ptr);
    }

    READ_WORD(&header.quick_generic_jni_trampoline_offset, ptr);
    READ_WORD(&header.quick_imt_conflict_trampoline_offset, ptr);
    READ_WORD(&header.quick_resolution_trampoline_offset, ptr);
    READ_WORD(&header.quick_to_interpreter_bridge_offset, ptr);
    READ_WORD(&header.image_patch_delta, ptr);
    READ_WORD(&header.image_file_location_oat_checksum, ptr);
    READ_WORD(&header.image_file_location_oat_data_begin, ptr);
    READ_WORD(&header.key_value_store_size, ptr);

    CHECK(header.common.magic == kOatMagicNum);
    return header;
  }

  void write(FileHandle& fh) {
    write_obj(fh, common);

    write_word(fh, static_cast<uint32_t>(instruction_set));
    write_word(fh, instruction_set_features_bitmap);
    write_word(fh, dex_file_count);

    if (common.version == static_cast<uint32_t>(OatVersion::V_131)) {
      write_word(fh, oat_dex_files_offset);
    }

    write_word(fh, executable_offset);
    write_word(fh, interpreter_to_interpreter_bridge_offset);
    write_word(fh, interpreter_to_compiled_code_bridge_offset);
    write_word(fh, jni_dlsym_lookup_offset);

    // These three fields are not present in version 064 and up.
    if (common.version == static_cast<uint32_t>(OatVersion::V_045) ||
        common.version == static_cast<uint32_t>(OatVersion::V_039)) {
      write_word(fh, portable_imt_conflict_trampoline_offset);
      write_word(fh, portable_resolution_trampoline_offset);
      write_word(fh, portable_to_interpreter_bridge_offset);
    }

    write_word(fh, quick_generic_jni_trampoline_offset);
    write_word(fh, quick_imt_conflict_trampoline_offset);
    write_word(fh, quick_resolution_trampoline_offset);
    write_word(fh, quick_to_interpreter_bridge_offset);
    write_word(fh, image_patch_delta);
    write_word(fh, image_file_location_oat_checksum);
    write_word(fh, image_file_location_oat_data_begin);
    write_word(fh, key_value_store_size);
  }

  void print() {
    printf(
        "OatHeader: {magic: 0x%08x, \
      version: 0x%08x, \
      checksum: 0x%08x, \
      isa: %s, \
      isa_features_bitmap: 0x%08x, \
      dex_file_count: 0x%08x, \
      executable_offset: 0x%08x, \
      interpreter_to_interpreter_bridge_offset: 0x%08x, \
      interpreter_to_compiled_code_bridge_offset: 0x%08x, \
      jni_dlsym_lookup_offset: 0x%08x",
        common.magic,
        common.version,
        common.adler32_checksum,
        instruction_set_str(instruction_set),
        instruction_set_features_bitmap,
        dex_file_count,
        executable_offset,
        interpreter_to_interpreter_bridge_offset,
        interpreter_to_compiled_code_bridge_offset,
        jni_dlsym_lookup_offset);

    if (common.version == static_cast<uint32_t>(OatVersion::V_045) ||
        common.version == static_cast<uint32_t>(OatVersion::V_039)) {
      printf(
          "portable_imt_conflict_trampoline_offset: 0x%08x, \
          portable_resolution_trampoline_offset: 0x%08x, \
          portable_to_interpreter_bridge_offset: 0x%08x",
          portable_imt_conflict_trampoline_offset,
          portable_resolution_trampoline_offset,
          portable_to_interpreter_bridge_offset);
    }

    printf(
        "quick_generic_jni_trampoline_offset: 0x%08x, \
        quick_imt_conflict_trampoline_offset: 0x%08x, \
        quick_resolution_trampoline_offset: 0x%08x, \
        quick_to_interpreter_bridge_offset: 0x%08x, \
        image_patch_delta: 0x%08x, \
        image_file_location_oat_checksum: 0x%08x, \
        image_file_location_oat_data_begin: 0x%08x, \
        key_value_store_size: 0x%08x}\n",
        quick_generic_jni_trampoline_offset,
        quick_imt_conflict_trampoline_offset,
        quick_resolution_trampoline_offset,
        quick_to_interpreter_bridge_offset,
        image_patch_delta,
        image_file_location_oat_checksum,
        image_file_location_oat_data_begin,
        key_value_store_size);
  }
};

// The oat file key-value store is a section of the oat file containing
// zero or more pairs of null-terminated strings.
class KeyValueStore {
 public:
  using KeyValue = std::pair<std::string, std::string>;

  explicit KeyValueStore(ConstBuffer buf) {
    cur_ma()->markBufferConsumed(buf);

    int remaining = buf.len;
    size_t next = 0;
    while (remaining > 0) {
      KeyValue kv;
      // key
      {
        auto len = strnlen(&buf[next], remaining) + 1;
        kv.first = std::string(&buf[next], len - 1);
        next += len;
        remaining -= len;
      }
      if (remaining <= 0) {
        break;
      }
      // value
      {
        auto len = strnlen(&buf[next], remaining) + 1;
        kv.second = std::string(&buf[next], len - 1);
        next += len;
        remaining -= len;
      }
      kv_pairs_.push_back(std::move(kv));
    }
  }

  void print() const {
    for (const auto& e : kv_pairs_) {
      printf("KeyValueStore: {%s: %s}\n", e.first.c_str(), e.second.c_str());
    }
  }

  static void write(FileHandle& fh, const std::vector<KeyValue>& kv_pairs) {
    for (const auto& e : kv_pairs) {
      write_str_and_null(fh, e.first);
      write_str_and_null(fh, e.second);
    }
  }

  static uint32_t compute_size(const std::vector<KeyValue>& kv_pairs) {
    uint32_t ret = 0;
    for (const auto& e : kv_pairs) {
      ret += e.first.size() + 1;
      ret += e.second.size() + 1;
    }
    return ret;
  }

  bool has_key(const std::string& key) const {
    for (const auto& kv : kv_pairs_) {
      if (kv.first == key) {
        return true;
      }
    }
    return false;
  }

  // return value remains valid as long as *this isn't destroyed.
  const char* get(const std::string& key) const {
    for (const auto& kv : kv_pairs_) {
      if (kv.first == key) {
        return kv.second.c_str();
      }
    }
    return nullptr;
  }

 private:
  std::vector<KeyValue> kv_pairs_;
};

// DexIdBufs handles looking up class names in dex files within in-memory oat
// files.
class DexIdBufs {
 public:
  // Note: DexIdBufs must not outlive the memory wrapped by oat_buf.
  DexIdBufs(ConstBuffer oat_buf,
            uint32_t dex_offset,
            const DexFileHeader& header) {
    dex_buf_ = oat_buf.slice(dex_offset);
    auto class_defs_buf = dex_buf_.slice(header.class_defs_off);
    auto type_ids_buf = dex_buf_.slice(header.type_ids_off);
    auto string_ids_buf = dex_buf_.slice(header.string_ids_off);
    auto method_ids_buf = dex_buf_.slice(header.method_ids_off);

    // We memcpy into new buffers since the data in the dex may not be aligned.
    class_defs_.reset(new DexClassDef[header.class_defs_size]);
    type_ids_.reset(new uint32_t[header.type_ids_size]);
    string_ids_.reset(new uint32_t[header.string_ids_size]);

    memcpy(class_defs_.get(),
           class_defs_buf.ptr,
           header.class_defs_size * sizeof(DexClassDef));
    memcpy(type_ids_.get(),
           type_ids_buf.ptr,
           header.type_ids_size * sizeof(uint32_t));
    memcpy(string_ids_.get(),
           string_ids_buf.ptr,
           header.string_ids_size * sizeof(uint32_t));

    // note: method ids are indexed by type, not class, hence must be size of
    // type_ids_size
    class_method_count_.resize(header.type_ids_size, 0);
    auto method_ids = std::make_unique<MethodId[]>(header.method_ids_size);
    memcpy(method_ids.get(),
           method_ids_buf.ptr,
           header.method_ids_size * sizeof(MethodId));
    for (unsigned int i = 0; i < header.method_ids_size; i++) {
      class_method_count_.at(method_ids[i].class_idx)++;
    }
  }

  int get_num_methods(int i) const {
    return class_method_count_[class_defs_[i].class_idx];
  }

  std::string get_class_name(int i) const {
    const auto class_idx = class_defs_[i].class_idx;
    const auto string_id = type_ids_[class_idx];
    const auto string_offset = string_ids_[string_id];

    auto string_buf = dex_buf_.slice(string_offset);
    char* ptr = const_cast<char*>(string_buf.ptr);
    const auto str_size = read_uleb128(&ptr) + 1;

    return std::string(ptr, str_size);
  }

 private:
  ConstBuffer dex_buf_;
  std::unique_ptr<DexClassDef[]> class_defs_;
  std::unique_ptr<uint32_t[]> type_ids_;
  std::unique_ptr<uint32_t[]> string_ids_;

  std::vector<int> class_method_count_;
};

// Class meta data for all the classes that appear in the dex files.
// ClassOffsets[0]...ClassOffsets[D] and OatClass[0]..OatClass[C] sections
// - DexFileListing (OatDexFile[0]...OatDexFile[D]) specifies the beginning of
//   the ClassOffsets for each dex file.
// - The class listing for a dex file is doubly indirect. It consists of an
//   array of offsets, whose length is specified by
//   DexFileHeader::class_defs_size. Each offset in that array points to
//   a single ClassInfo struct for that class.
class OatClasses {
 public:
  enum class Status {
    kStatusRetired = -2,
    kStatusError = -1,
    kStatusNotReady = 0,
    kStatusIdx = 1,
    kStatusLoaded = 2,
    kStatusResolving = 3,
    kStatusResolved = 4,
    kStatusVerifying = 5,
    kStatusRetryVerificationAtRuntime = 6,
    kStatusVerifyingAtRuntime = 7,
    kStatusVerified = 8,
    kStatusInitializing = 9,
    kStatusInitialized = 10,
    kStatusMax = 11,
  };

  enum class Type {
    kOatClassAllCompiled = 0,
    kOatClassSomeCompiled = 1,
    kOatClassNoneCompiled = 2,
    kOatClassMax = 3,
  };

  static const char* statusStr(Status status) {
    switch (status) {
    case Status::kStatusRetired:
      return "kStatusRetired";
    case Status::kStatusError:
      return "kStatusError";
    case Status::kStatusNotReady:
      return "kStatusNotReady";
    case Status::kStatusIdx:
      return "kStatusIdx";
    case Status::kStatusLoaded:
      return "kStatusLoaded";
    case Status::kStatusResolving:
      return "kStatusResolving";
    case Status::kStatusResolved:
      return "kStatusResolved";
    case Status::kStatusVerifying:
      return "kStatusVerifying";
    case Status::kStatusRetryVerificationAtRuntime:
      return "kStatusRetryVerificationAtRuntime";
    case Status::kStatusVerifyingAtRuntime:
      return "kStatusVerifyingAtRuntime";
    case Status::kStatusVerified:
      return "kStatusVerified";
    case Status::kStatusInitializing:
      return "kStatusInitializing";
    case Status::kStatusInitialized:
      return "kStatusInitialized";
    case Status::kStatusMax:
      return "kStatusMax";
    default:
      return "<UNKNOWN>";
    }
  }

  static const char* statusStr(int status) {
    return statusStr(static_cast<Status>(status));
  }

  static const char* shortStatusStr(Status status) {
    switch (status) {
    case Status::kStatusRetired:
      return "O";
    case Status::kStatusError:
      return "E";
    case Status::kStatusNotReady:
      return "N";
    case Status::kStatusIdx:
      return "I";
    case Status::kStatusLoaded:
      return "L";
    case Status::kStatusResolving:
      return "r";
    case Status::kStatusResolved:
      return "R";
    case Status::kStatusVerifying:
      return "v";
    case Status::kStatusRetryVerificationAtRuntime:
      return "v";
    case Status::kStatusVerifyingAtRuntime:
      return "v";
    case Status::kStatusVerified:
      return "V";
    case Status::kStatusInitializing:
      return "i";
    case Status::kStatusInitialized:
      return "I";
    case Status::kStatusMax:
      return "M";
    default:
      return "?";
    }
  }

  static const char* typeStr(Type type) {
    switch (type) {
    case Type::kOatClassAllCompiled:
      return "kOatClassAllCompiled";
    case Type::kOatClassSomeCompiled:
      return "kOatClassSomeCompiled";
    case Type::kOatClassNoneCompiled:
      return "kOatClassNoneCompiled";
    case Type::kOatClassMax:
      return "kOatClassMax";
    default:
      return "<UKNOWN>";
    }
  }

  static const char* shortTypeStr(Type type) {
    switch (type) {
    case Type::kOatClassAllCompiled:
      return "C";
    case Type::kOatClassSomeCompiled:
      return "c";
    case Type::kOatClassNoneCompiled:
      return "n";
    case Type::kOatClassMax:
      return "M";
    default:
      return "?";
    }
  }

  // Note that this only handles uncompiled classes. Compiled classes
  // additionally contain a bitmap for each method, along with a field
  // specifying the length of the bitmap.
  struct PACK ClassInfo {
    ClassInfo() = default;
    ClassInfo(Status s, Type t)
        : status(static_cast<int16_t>(s)), type(static_cast<uint16_t>(t)) {}
    int16_t status = 0;
    uint16_t type = 0;
  };

 protected:
  OatClasses() = default;
};

class DexFileListing {
 public:
  struct DexFile {
    DexFile() = default;
    DexFile(const std::string& location_,
            uint32_t location_checksum_,
            uint32_t file_offset_)
        : location(location_),
          location_checksum(location_checksum_),
          file_offset(file_offset_) {}

    std::string location;
    uint32_t location_checksum;
    uint32_t file_offset;
  };

  virtual std::vector<uint32_t> dex_file_offsets() const = 0;
  virtual ~DexFileListing() = default;
};

// Dex File listing for OAT versions 079, 088.
//
// Meta data about dex files, comes immediately after the KeyValueStore
// OatHeader::dex_file_count specifies how many entries there are in the
// listing.
//
// Each listing consists of:
//
//    location_length:     4 unsigned bytes.
//    location:            un-terminated character string, length specified by
//                         location_length
//    location_checksum:   4 unsigned bytes, checksum of location.
//    file_offset:         4 unsigned bytes, offset from beginning of OAT file
//                         where the specified dex file begins.
//    classes_offset:      4 unsigned bytes, offset from beginning of OAT file
//                         where class metadata listing (OatClasses) for this
//                         dex file begins.
//    lookup_table_offset: 4 unsigned bytes, offset from beginning of OAT file
//                         where the class lookup table (LookupTables) for this
//                         dex file begins.
class DexFileListing_079 : public DexFileListing {
 public:
  MOVABLE(DexFileListing_079);

  struct DexFile_079 : public DexFile {
    DexFile_079() = default;
    DexFile_079(const std::string& location_,
                uint32_t location_checksum_,
                uint32_t file_offset_,
                uint32_t num_classes_,
                uint32_t classes_offset_,
                uint32_t lookup_table_offset_)
        : DexFile(location_, location_checksum_, file_offset_),
          num_classes(num_classes_),
          classes_offset(classes_offset_),
          lookup_table_offset(lookup_table_offset_) {}

    uint32_t num_classes;
    uint32_t classes_offset;
    uint32_t lookup_table_offset;
  };

  DexFileListing_079(int numDexFiles, ConstBuffer buf) {
    auto ptr = buf.ptr;
    while (numDexFiles > 0) {
      numDexFiles--;

      DexFile_079 file;
      uint32_t location_len = 0;
      READ_WORD(&location_len, ptr);

      file.location = std::string(ptr, location_len);
      cur_ma()->markRangeConsumed(ptr, location_len);
      ptr += location_len;

      READ_WORD(&file.location_checksum, ptr);
      READ_WORD(&file.file_offset, ptr);
      READ_WORD(&file.classes_offset, ptr);
      READ_WORD(&file.lookup_table_offset, ptr);

      dex_files_.push_back(file);
    }
  }

  void print() {
    size_t i = 0;
    for (const auto& e : dex_files_) {
      printf(
          "OatDexFile[%zu]: {location: %s, \
        location_checksum: 0x%08x, \
        file_offset: 0x%08x, \
        classes_offset: 0x%08x, \
        lookup_table_offset: 0x%08x}\n",
          i,
          e.location.c_str(),
          e.location_checksum,
          e.file_offset,
          e.classes_offset,
          e.lookup_table_offset);
      i++;
    }
  }

  const std::vector<DexFile_079>& dex_files() const { return dex_files_; }

  std::vector<uint32_t> dex_file_offsets() const override {
    std::vector<uint32_t> ret;
    for (const auto& f : dex_files_) {
      ret.push_back(f.file_offset);
    }
    return ret;
  }

  static uint32_t compute_size(const std::vector<DexInput>& dex_input,
                               bool /*samsung_mode*/) {

    // Locations are *not* null terminated.
    const auto num_files = dex_input.size();
    uint32_t total_file_location_size = 0;
    for (const auto& e : dex_input) {
      total_file_location_size += e.location.size();
    }
    return total_file_location_size +
           num_files * sizeof(uint32_t) + // location len
           num_files * sizeof(uint32_t) + // location checksum
           num_files * sizeof(uint32_t) + // file offset
           num_files * sizeof(uint32_t) + // classes offset
           num_files * sizeof(uint32_t); // lookup_table_offset
  }

  static std::vector<DexFile_079> build(const std::vector<DexInput>& dexes,
                                        uint32_t& next_offset,
                                        bool samsung_mode);

  static void write(
      FileHandle& fh,
      const std::vector<DexFileListing_079::DexFile_079>& dex_files,
      bool /*samsung_mode*/) {
    for (const auto& file : dex_files) {
      uint32_t location_len = file.location.size();
      write_word(fh, location_len);

      ConstBuffer location{file.location.c_str(), location_len};
      // Locations are *not* null terminated.
      write_buf(fh, location);
      write_word(fh, file.location_checksum);
      write_word(fh, file.file_offset);
      write_word(fh, file.classes_offset);
      write_word(fh, file.lookup_table_offset);
    }
  }

 private:
  std::vector<DexFile_079> dex_files_;
};

class DexFileListing_124 : public DexFileListing_079 {
 public:
  using DexFile_124 = DexFile_079;

  MOVABLE(DexFileListing_124);
  DexFileListing_124(int numDexFiles, ConstBuffer buf)
      : DexFileListing_079(numDexFiles, buf) {
    CHECK(numDexFiles <= 1,
          "For V124/V131 we only support one dex per odex/vdex pair");
  }

  static std::vector<DexFile_124> build(const std::vector<DexInput>& dexes,
                                        uint32_t& next_offset,
                                        bool samsung_mode);
};

class DexFileListing_131 : public DexFileListing_124 {
 public:
  struct DexFile_131 : public DexFile_124 {
    DexFile_131() = default;
    DexFile_131(const std::string& location_,
                uint32_t location_checksum_,
                uint32_t file_offset_,
                uint32_t num_classes_,
                uint32_t classes_offset_,
                uint32_t lookup_table_offset_,
                uint32_t dex_layout_sections_offset_,
                uint32_t method_bss_mapping_offset_)
        : DexFile_124(location_,
                      location_checksum_,
                      file_offset_,
                      num_classes_,
                      classes_offset_,
                      lookup_table_offset_),
          dex_layout_sections_offset(dex_layout_sections_offset_),
          method_bss_mapping_offset(method_bss_mapping_offset_) {}

    uint32_t dex_layout_sections_offset;
    uint32_t method_bss_mapping_offset;
  };

  MOVABLE(DexFileListing_131);
  DexFileListing_131(int numDexFiles, ConstBuffer buf)
      : DexFileListing_124(numDexFiles, buf) {
    CHECK(numDexFiles <= 1,
          "For V124/V131 we only support one dex per odex/vdex pair");
  }

  static uint32_t compute_size(const std::vector<DexInput>& dex_input,
                               bool /*samsung_mode*/) {
    // Locations are *not* null terminated.
    const auto num_files = dex_input.size();
    uint32_t total_file_location_size = 0;
    for (const auto& e : dex_input) {
      total_file_location_size += e.location.size();
    }
    return num_files * sizeof(uint32_t) + // location len
           total_file_location_size +
           num_files * sizeof(uint32_t) + // location checksum
           num_files * sizeof(uint32_t) + // dex file offset
           num_files * sizeof(uint32_t) + // classes offset
           num_files * sizeof(uint32_t) + // lookup_table_offset
           num_files * sizeof(uint32_t) + // dex_layout_sections_offset
           num_files * sizeof(uint32_t); // method_bss_mapping_offset
  }

  static std::vector<DexFile_131> build(const std::vector<DexInput>& dexes,
                                        uint32_t& next_offset,
                                        bool samsung_mode);

  static void write(
      FileHandle& fh,
      const std::vector<DexFileListing_131::DexFile_131>& dex_files,
      bool /*samsung_mode*/) {
    for (const auto& file : dex_files) {
      uint32_t location_len = file.location.size();
      write_word(fh, location_len);

      ConstBuffer location{file.location.c_str(), location_len};
      // Locations are *not* null terminated.
      write_buf(fh, location);
      write_word(fh, file.location_checksum);
      write_word(fh, file.file_offset);
      write_word(fh, file.classes_offset);
      write_word(fh, file.lookup_table_offset);
      write_word(fh, file.dex_layout_sections_offset);
      write_word(fh, file.method_bss_mapping_offset);

#ifdef DEBUG_LOG

      printf(
          "WRITING DexFileListing_131: \
          location_len: %u\
          location: %s\
          location_checksum: %04x\
          file_offset: %u\
          classes_offset: %u\
          lookup_table_offset: %u\
          dex_layout_sections_offset: %u\
          method_bss_mapping_offset: %u\n",
          location_len,
          file.location.c_str(),
          file.location_checksum,
          file.file_offset,
          file.classes_offset,
          file.lookup_table_offset,
          file.dex_layout_sections_offset,
          file.method_bss_mapping_offset);
#endif
    }
  }
};

// Dex File listing for OAT versions 064 and 045.
//
// Meta data about dex files, comes immediately after the KeyValueStore
// OatHeader::dex_file_count specifies how many entries there are in the
// listing.
//
// Each listing consists of:
//
//    location_length:     4 unsigned bytes.
//    location:            un-terminated character string, length specified by
//                         location_length
//    location_checksum:   4 unsigned bytes, checksum of location.
//    file_offset:         4 unsigned bytes, offset from beginning of OAT file
//                         where the specified dex file begins.
//    classes:             Variable length table of offsets pointing to class
//                         status information. Length depends on the number of
//                         classes in the dex file.
//
// The offsets in `classes` point to ClassInfo structs. If the value a
// ClassInfo's type field is kOatClassSomeCompiled, then the ClassInfo is
// followed by:
//
//   - 4 bytes containing a bitmask size.
//   - N bytes of bitmask, where N is specified in the previous field.
//   - M 4 byte method pointers, where M is equal to the total number of set
//     bits in the bitmask.
//
// If the type field is kOatClassAllCompiled, then the ClassInfo is followed by
//   - M 4 byte methods pointers, where M is the number of methods in the given
//     class.
//
// Otherwise, there is no additional data after ClassInfo.
class DexFileListing_064 : public DexFileListing {
 public:
  MOVABLE(DexFileListing_064);

  using ClassInfo = OatClasses::ClassInfo;

  struct DexFile_064 : public DexFile {
    DexFile_064() = default;
    DexFile_064(const std::string& location_,
                uint32_t location_checksum_,
                uint32_t file_offset_,
                uint32_t lookup_table_offset_,
                const std::vector<uint32_t>& class_offsets_,
                const std::vector<ClassInfo>& class_info_)
        : DexFile(location_, location_checksum_, file_offset_),
          lookup_table_offset(lookup_table_offset_),
          class_offsets(class_offsets_),
          class_info(class_info_) {}

    uint32_t lookup_table_offset;
    std::vector<uint32_t> class_offsets;
    std::vector<ClassInfo> class_info;
    std::vector<std::string> class_names;
  };

  DexFileListing_064(bool dex_files_only,
                     OatVersion version,
                     int numDexFiles,
                     ConstBuffer buf,
                     ConstBuffer oat_buf) {
    auto oat_method_offset_size = 0;
    if (version == OatVersion::V_039) {
      // http://androidxref.com/5.0.0_r2/xref/art/runtime/oat.h#161
      oat_method_offset_size = 8;
    } else if (version == OatVersion::V_045 || version == OatVersion::V_064 ||
               version == OatVersion::V_067) {
      // http://androidxref.com/5.1.1_r6/xref/art/runtime/oat.h#163
      oat_method_offset_size = 4;
    } else {
      CHECK(false, "Invalid oat version for DexFileListing_064");
    }

    auto ptr = buf.ptr;
    while (numDexFiles > 0) {
      numDexFiles--;

      DexFile_064 file;
      uint32_t location_len = 0;
      READ_WORD(&location_len, ptr);

      file.location = std::string(ptr, location_len);
      cur_ma()->markRangeConsumed(ptr, location_len);
      ptr += location_len;

      READ_WORD(&file.location_checksum, ptr);
      READ_WORD(&file.file_offset, ptr);

      // Samsung has an extra field here, which is the offset to their custom
      // type lookup table. It comes before the dex, whereas the class info
      // tables come after the dex, so we can always detect this field based on
      // comparing it to the dex file offset.
      uint32_t next_word;
      memcpy(&next_word, ptr, 4);
      if (next_word < file.file_offset) {
        is_samsung_ = true;
        READ_WORD(&file.lookup_table_offset, ptr);
      } else {
        file.lookup_table_offset = 0;
      }

      auto dex_header = DexFileHeader::parse(oat_buf.slice(file.file_offset));
      const auto num_classes = dex_header.class_defs_size;
      if (!dex_files_only) {
        file.class_info.reserve(num_classes);

        DexIdBufs id_bufs(oat_buf, file.file_offset, dex_header);

        for (unsigned int i = 0; i < num_classes; i++) {
          uint32_t class_info_offset;
          READ_WORD(&class_info_offset, ptr);

          ClassInfo class_info;
          cur_ma()->memcpyAndMark(
              &class_info, oat_buf.ptr + class_info_offset, sizeof(ClassInfo));

          // Note: So far I haven't found this pattern in version 064, so I'm
          // not 100% sure this will work for 064. It definitely works for 045,
          // where this pattern appears to occur more frequently.
          if (class_info.type ==
              static_cast<uint16_t>(OatClasses::Type::kOatClassSomeCompiled)) {
            auto bitmap_size_ptr =
                oat_buf.ptr + class_info_offset + sizeof(ClassInfo);
            uint32_t bitmap_size = 0;
            cur_ma()->memcpyAndMark(
                &bitmap_size, bitmap_size_ptr, sizeof(uint32_t));
            auto bitmap_ptr = bitmap_size_ptr + sizeof(uint32_t);
            cur_ma()->markRangeConsumed(bitmap_ptr, bitmap_size);

            int method_count = 0;
            for (unsigned int j = 0; j < (bitmap_size / 4); j++) {
              uint32_t bitmap_element = 0;
              READ_WORD(&bitmap_element, bitmap_ptr);
              method_count += countSetBits(bitmap_element);
            }

            auto methods_ptr = bitmap_ptr;
            cur_ma()->markRangeConsumed(methods_ptr,
                                        method_count * oat_method_offset_size);

          } else if (class_info.type ==
                     static_cast<uint16_t>(
                         OatClasses::Type::kOatClassAllCompiled)) {
            auto method_count = id_bufs.get_num_methods(i);
            auto methods_ptr =
                oat_buf.ptr + class_info_offset + sizeof(ClassInfo);
            cur_ma()->markRangeConsumed(methods_ptr,
                                        method_count * oat_method_offset_size);
          }

          file.class_info.push_back(class_info);
          file.class_names.push_back(id_bufs.get_class_name(i));
        }
      } else {
        // must consume the class info.
        for (unsigned int i = 0; i < num_classes; i++) {
          uint32_t class_info_offset;
          READ_WORD(&class_info_offset, ptr);
        }
      }

      dex_files_.push_back(file);
    }
  }

  void print() {
    for (const auto& e : dex_files_) {
      printf("  {\n");
      printf("    location: %s\n", e.location.c_str());
      printf("    location_checksum: 0x%08x\n", e.location_checksum);
      printf("    file_offset: 0x%08x\n", e.file_offset);
      printf("  }\n");
    }
  }

  void print_classes() {
    for (const auto& e : dex_files_) {
      printf("  { Classes for dex %s\n", e.location.c_str());
      int count = 0;
      for (const auto& info : e.class_info) {
        if (count == 0) {
          printf("    ");
        }
        printf(
            "%s%s ",
            OatClasses::shortStatusStr(
                static_cast<OatClasses::Status>(info.status)),
            OatClasses::shortTypeStr(static_cast<OatClasses::Type>(info.type)));
        count++;
        if (count >= 32) {
          printf("\n");
          count = 0;
        }
      }
      printf("  }\n");
    }
  }

  void print_unverified_classes() {
    printf("unverified classes:\n");
    for (const auto& e : dex_files_) {
      printf("  %s\n", e.location.c_str());
      foreach_pair(
          e.class_info,
          e.class_names,
          [&](const ClassInfo& info, const std::string& name) {
            if (info.status <
                static_cast<int>(OatClasses::Status::kStatusVerified)) {
              printf("    %s unverified (status: %s)\n",
                     name.c_str(),
                     OatClasses::statusStr(info.status));
            }
          });
    }
  }

  std::vector<uint32_t> dex_file_offsets() const override {
    std::vector<uint32_t> ret;
    for (const auto& f : dex_files_) {
      ret.push_back(f.file_offset);
    }
    return ret;
  }

  const std::vector<DexFile_064>& dex_files() const { return dex_files_; }

  static uint32_t compute_size(const std::vector<DexInput>& dex_input,
                               bool samsung_mode) {

    // Locations are *not* null terminated.
    const auto num_files = dex_input.size();
    uint32_t total_file_location_size = 0;
    uint32_t total_class_data_size = 0;
    for (const auto& e : dex_input) {
      total_file_location_size += e.location.size();

      auto dex_fh = FileHandle(fopen(e.filename.c_str(), "r"));
      CHECK(dex_fh.get() != nullptr);

      auto file_size = get_filesize(dex_fh);
      CHECK(file_size >= sizeof(DexFileHeader));

      // read the header to get the count of classes.
      DexFileHeader header = {};
      CHECK(dex_fh.fread(&header, sizeof(DexFileHeader), 1) == 1);

      total_class_data_size += header.class_defs_size * sizeof(uint32_t);
    }
    auto samsung_table_offset_size =
        samsung_mode ? num_files * sizeof(uint32_t) : 0;
    return total_file_location_size + total_class_data_size +
           num_files * sizeof(uint32_t) + // location len
           num_files * sizeof(uint32_t) + // location checksum
           num_files * sizeof(uint32_t) + // file offset
           samsung_table_offset_size;
  }

  static std::vector<DexFileListing_064::DexFile_064> build(
      const std::vector<DexInput>& dex_input,
      uint32_t& next_offset,
      bool samsung_mode);

  static void write(
      FileHandle& fh,
      const std::vector<DexFileListing_064::DexFile_064>& dex_files,
      bool samsung_mode) {
    for (const auto& file : dex_files) {
      uint32_t location_len = file.location.size();
      write_word(fh, location_len);

      ConstBuffer location{file.location.c_str(), location_len};
      // Locations are *not* null terminated.
      write_buf(fh, location);
      write_word(fh, file.location_checksum);
      write_word(fh, file.file_offset);
      if (samsung_mode) {
        write_word(fh, file.lookup_table_offset);
      }
      write_vec(fh, file.class_offsets);
    }
  }

  bool is_samsung() const { return is_samsung_; }

 private:
  std::vector<DexFile_064> dex_files_;

  bool is_samsung_ = false;
};

// Collection of all the headers of all the dex files found in the oat.
class DexFiles {
 public:
  MOVABLE(DexFiles);

  // buf should start at the beginning of the OAT file, as the offsets
  // in DexFileListing are relative to the beginning of the OAT file.
  DexFiles(const DexFileListing& dex_file_listing, ConstBuffer buf) {
    for (const auto& file_offset : dex_file_listing.dex_file_offsets()) {
      auto dex_header_buf = buf.slice(file_offset);
      auto dh = DexFileHeader::parse(dex_header_buf);
      headers_.push_back(dh);

      auto dex_buf = buf.slice(file_offset, file_offset + dh.file_size);
      dexes_.push_back(dex_buf);
    }
  }

  void print() {
    for (const auto& e : headers_) {
      printf(
          "DexFile: { \
      file_size: 0x%08x(%u), \
      num_classes: 0x%08x(%u)}\n",
          e.file_size,
          e.file_size,
          e.class_defs_size,
          e.class_defs_size);
    }
    size_t index = 0;
    for (const auto& e : dexes_) {
      print_dex_opcodes(reinterpret_cast<const uint8_t*>(e.ptr),
                        headers_[index].file_size);
      index++;
    }
  }

  const std::vector<DexFileHeader>& headers() const { return headers_; }

 private:
  std::vector<DexFileHeader> headers_;
  std::vector<ConstBuffer> dexes_;
};

class OatClasses_079 : public OatClasses {
 public:
  struct DexClasses {
    std::string dex_file;
    std::vector<ClassInfo> class_info;
    std::vector<std::string> class_names;
  };

  OatClasses_079() = default;

  OatClasses_079(const DexFileListing_079& dex_file_listing,
                 const DexFiles& dex_files,
                 ConstBuffer oat_buf);

  MOVABLE(OatClasses_079);

  void print();

  void print_unverified_classes();

  template <typename DexFileType>
  static void write(const std::vector<DexFileType>& dex_files,
                    FileHandle& cksum_fh);

 protected:
  std::vector<DexClasses> classes_;
};

class OatClasses_124 : public OatClasses_079 {
 public:
  OatClasses_124() = default;
  OatClasses_124(const DexFileListing_079& dex_file_listing,
                 const DexFiles& dex_files,
                 ConstBuffer oat_buf,
                 ConstBuffer dex_buf);
};

OatClasses_124::OatClasses_124(const DexFileListing_079& dex_file_listing,
                               const DexFiles& dex_files,
                               ConstBuffer oat_buf,
                               ConstBuffer dex_buf) {
  foreach_pair(dex_file_listing.dex_files(),
               dex_files.headers(),
               [&](const DexFileListing_079::DexFile_079& listing,
                   const DexFileHeader& header) {
                 auto classes_offset = listing.classes_offset;

                 DexClasses dex_classes;
                 dex_classes.dex_file = listing.location;

                 DexIdBufs id_bufs(dex_buf, listing.file_offset, header);

                 // classes_offset points to an array of pointers (offsets) to
                 // ClassInfo
                 for (unsigned int i = 0; i < header.class_defs_size; i++) {

                   ClassInfo info;
                   uint32_t info_offset;
                   cur_ma()->memcpyAndMark(
                       &info_offset,
                       oat_buf.slice(classes_offset + i * sizeof(uint32_t)).ptr,
                       sizeof(uint32_t));
                   cur_ma()->memcpyAndMark(&info,
                                           oat_buf.slice(info_offset).ptr,
                                           sizeof(ClassInfo));

                   // TODO: Handle compiled classes. Need to read method bitmap
                   // size, and method bitmap.
                   dex_classes.class_info.push_back(info);
                   dex_classes.class_names.push_back(id_bufs.get_class_name(i));
                 }
                 classes_.push_back(dex_classes);
               });
}

class OatClasses_064 : public OatClasses {
 public:
  static void write(
      const std::vector<DexFileListing_064::DexFile_064>& dex_files,
      FileHandle& cksum_fh) {
    // offsets were already written to the DexFileListing_064.
    for (const auto& file : dex_files) {
      if (file.class_offsets.empty()) {
        continue;
      }
      CHECK(file.class_offsets[0] == cksum_fh.bytes_written());
      for (const auto& info : file.class_info) {
        write_obj(cksum_fh, info);
      }
    }
  }
};

OatClasses_079::OatClasses_079(const DexFileListing_079& dex_file_listing,
                               const DexFiles& dex_files,
                               ConstBuffer oat_buf) {
  foreach_pair(
      dex_file_listing.dex_files(),
      dex_files.headers(),
      [&](const DexFileListing_079::DexFile_079& listing,
          const DexFileHeader& header) {
        auto classes_offset = listing.classes_offset;

        DexClasses dex_classes;
        dex_classes.dex_file = listing.location;

        DexIdBufs id_bufs(oat_buf, listing.file_offset, header);

        // classes_offset points to an array of pointers (offsets) to
        // ClassInfo
        for (unsigned int i = 0; i < header.class_defs_size; i++) {

          ClassInfo info;
          uint32_t info_offset;
          cur_ma()->memcpyAndMark(
              &info_offset,
              oat_buf.slice(classes_offset + i * sizeof(uint32_t)).ptr,
              sizeof(uint32_t));
          cur_ma()->memcpyAndMark(
              &info, oat_buf.slice(info_offset).ptr, sizeof(ClassInfo));

          // TODO: Handle compiled classes. Need to read method bitmap size,
          // and method bitmap.
          CHECK(info.type == static_cast<uint16_t>(Type::kOatClassNoneCompiled),
                "Parsing for compiled classes not implemented");

          dex_classes.class_info.push_back(info);
          dex_classes.class_names.push_back(id_bufs.get_class_name(i));
        }
        classes_.push_back(dex_classes);
      });
}

void OatClasses_079::print() {
  for (const auto& e : classes_) {
    printf("  { Classes for dex %s\n", e.dex_file.c_str());

    int count = 0;
    for (const auto& info : e.class_info) {
      if (count == 0) {
        printf("    ");
      }
      printf("%s%s ",
             shortStatusStr(static_cast<Status>(info.status)),
             shortTypeStr(static_cast<Type>(info.type)));
      count++;
      if (count >= 32) {
        printf("\n");
        count = 0;
      }
    }
    printf("  }\n");
  }
}

void OatClasses_079::print_unverified_classes() {
  printf("unverified classes:\n");
  for (const auto& e : classes_) {
    printf("  %s\n", e.dex_file.c_str());
    foreach_pair(e.class_info,
                 e.class_names,
                 [&](const ClassInfo& info, const std::string& name) {
                   if (info.status <
                       static_cast<int>(Status::kStatusVerified)) {
                     printf("    %s unverified (status: %s)\n",
                            name.c_str(),
                            statusStr(info.status));
                   }
                 });
  }
}

template <typename DexFileType>
void OatClasses_079::write(const std::vector<DexFileType>& dex_files,
                           FileHandle& cksum_fh) {
#ifdef DEBUG_LOG
  printf("WRITING OatClasses:\n");
#endif

  size_t dex_count = 0;
  for (const auto& dex_file : dex_files) {
    CHECK(dex_file.classes_offset == cksum_fh.bytes_written());

    const auto num_classes = dex_file.num_classes;
    uint32_t table_offset =
        dex_file.classes_offset + num_classes * sizeof(uint32_t);

#ifdef DEBUG_LOG
    printf(
        "WRITING OatClasses for dex[%zu]: \
      #classes: %u :: \
      #offset: %u (-> %u)\n",
        dex_count,
        num_classes,
        dex_file.classes_offset,
        table_offset);
#endif

    // write pointers to ClassInfo.
    for (size_t i = 0; i < num_classes; i++) {
      write_word(cksum_fh, table_offset + i * sizeof(uint32_t));

#ifdef DEBUG_LOG
      printf("#ClassOffsets[%zu] -> %zu\n",
             i,
             table_offset + i * sizeof(uint32_t));
#endif
    }
    CHECK(table_offset == cksum_fh.bytes_written());

    // Write ClassInfo structs.
    OatClasses::ClassInfo info(OatClasses::Status::kStatusVerified,
                               OatClasses::Type::kOatClassNoneCompiled);
    for (size_t i = 0; i < num_classes; i++) {
      write_obj(cksum_fh, info);

#ifdef DEBUG_LOG
      printf("#OatClass[%zu]:%u ::  type: %u\n", i, table_offset, info.type);
#endif
      table_offset += sizeof(OatClasses::ClassInfo);
    }
    CHECK(table_offset == cksum_fh.bytes_written());
    dex_count++;
  }
}

class SamsungLookupTablesNil {
 public:
  template <typename DexFileListingType>
  static void write(const std::vector<DexInput>&,
                    const DexFileListingType&,
                    FileHandle&) {}
};

// Code to generate the lookup tables used on Samsung 5.0 phones.
//
// This is very similar to the LookupTables class, however, almost all the
// details are slightly different (e.g., same hash function, but samsung starts
// the hash at 1, instead of 0). As such there's not any value in trying to
// factor any common code out here, it would just result in a huge mess.
class SamsungLookupTables {
 public:
  MOVABLE(SamsungLookupTables);

  struct LookupTableEntry {
    uint32_t hash;
    uint32_t str_offset;
    uint32_t type_index;
  };

  struct LookupTable {
    std::unique_ptr<LookupTableEntry[]> data;
    uint32_t size;

    size_t byte_size() const { return size * sizeof(LookupTableEntry); }
  };

  static bool supportedSize(uint32_t num_class_defs) {
    return num_class_defs != 0u &&
           num_class_defs <= std::numeric_limits<uint16_t>::max();
  }

  static uint32_t numEntries(uint32_t num_classes) {
    return supportedSize(num_classes) ? roundUpToPowerOfTwo(num_classes) : 0u;
  }

  static uint32_t rawSize(uint32_t num_classes) {
    return numEntries(num_classes) * sizeof(LookupTableEntry);
  }

  static void write(
      const std::vector<DexInput>& dex_input_vec,
      const std::vector<DexFileListing_064::DexFile_064>& dex_files,
      FileHandle& cksum_fh) {
    foreach_pair(
        dex_input_vec,
        dex_files,
        [&](const DexInput& dex_input,
            const DexFileListing_064::DexFile_064& dex_file) {
          CHECK(dex_file.lookup_table_offset == cksum_fh.bytes_written());

          auto table = build_lookup_table(dex_input.filename);

          auto buf =
              ConstBuffer{reinterpret_cast<const char*>(table.data.get()),
                          table.byte_size()};
          write_buf(cksum_fh, buf);
        });
  }

 private:
  static void insert(LookupTableEntry* table,
                     uint32_t lookup_table_size,
                     uint32_t hash,
                     uint32_t string_offset,
                     uint16_t value) {
    const auto mask = lookup_table_size - 1;
    const auto start_bucket = hash & mask;
    auto bucket = start_bucket;

    do {
      auto& entry = table[bucket];
      if (entry.str_offset == 0) {
        entry.hash = hash;
        entry.str_offset = string_offset;
        entry.type_index = value;
        return;
      }

      bucket = (bucket + 1) & mask;
    } while (bucket != start_bucket);

    // since the size of the table is chosen to be larger than the number of
    // items to insert, this should never happen.
    fprintf(stderr, "Error: ran out of hash table space");
  }

  static uint32_t hash_str(const std::string& str) {
    uint32_t hash = 1;
    const char* chars = str.c_str();
    while (*chars != '\0') {
      hash = hash * 31 + *chars;
      chars++;
    }
    return hash;
  }

  static LookupTable build_lookup_table(const std::string& filename) {

    auto dex_fh = FileHandle(fopen(filename.c_str(), "r"));

    DexFileHeader header = {};
    CHECK(dex_fh.fread(&header, sizeof(DexFileHeader), 1) == 1);

    const auto num_type_ids = header.type_ids_size;

    const auto lookup_table_size = numEntries(num_type_ids);

    std::unique_ptr<LookupTableEntry[]> table_buf(
        new LookupTableEntry[lookup_table_size]);

    memset(table_buf.get(), 0, lookup_table_size * sizeof(LookupTableEntry));

    // Read type ids array.
    std::unique_ptr<uint32_t[]> typeid_buf(new uint32_t[num_type_ids]);
    CHECK(dex_fh.seek_set(header.type_ids_off));
    CHECK(dex_fh.fread(typeid_buf.get(), sizeof(uint32_t), num_type_ids) ==
          num_type_ids);

    // Read the string ids array.
    const auto num_string_ids = header.string_ids_size;
    std::unique_ptr<uint32_t[]> stringid_buf(new uint32_t[num_string_ids]);
    CHECK(dex_fh.seek_set(header.string_ids_off));
    CHECK(dex_fh.fread(stringid_buf.get(), sizeof(uint32_t), num_string_ids) ==
          num_string_ids);

    constexpr int kTypeNameBufSize = 256;

    char type_name_buf[kTypeNameBufSize] = {};

    struct Retry {
      uint32_t string_offset;
      uint16_t data;
      uint32_t hash;
    };

    std::vector<Retry> retry_indices;

    for (unsigned int i = 0; i < num_type_ids; i++) {
      const auto string_id = typeid_buf[i];
      CHECK(string_id < num_string_ids);

      const auto string_offset = stringid_buf[string_id];

      dex_fh.seek_set(string_offset);
      auto read_size =
          dex_fh.fread(type_name_buf, sizeof(char), kTypeNameBufSize);
      CHECK(read_size > 0);

      auto ptr = type_name_buf;
      const auto str_size = read_uleb128(&ptr) + 1;
      const auto str_start = ptr - type_name_buf;

      std::string type_name;

      if (str_start + str_size >= kTypeNameBufSize) {
        std::unique_ptr<char[]> large_class_name_buf(new char[str_size]);
        dex_fh.seek_set(string_offset + str_start);
        CHECK(dex_fh.fread(large_class_name_buf.get(),
                           sizeof(char),
                           str_size) == str_size);
        type_name = std::string(large_class_name_buf.get(), str_size);
      } else {
        type_name = std::string(ptr, str_size);
      }

      const auto hash = hash_str(type_name);
      insert(table_buf.get(), lookup_table_size, hash, string_offset, i);
    }

    return LookupTable{std::move(table_buf), lookup_table_size};
  }
};

// Type lookup tables for all dex files in the oat file.
// - The beginning offset of the lookup-table for the dex is specified in
//   the DexFileListing.
// - The number of entries in the table is equal to the first power of 2
//   which is larger than the number of classes in the dex file
//   (DexFileHeader::class_defs_size).
// - Each entry in the table is a LookupTableEntry struct.
class LookupTables {
 public:
  MOVABLE(LookupTables);

  // LookupTableEntry is exactly the layout of the entry in the file.
  struct LookupTableEntry {
    uint32_t str_offset; // the offset, relative to the beginning of the
                         // dexfile, where the name of the class begins.
    uint16_t data;
    uint16_t next_pos_delta;
  };

  // A mostly-materialized view for a single table - entries points
  // directly into the buffer.
  struct LookupTable {
    uint32_t dex_file_offset;
    std::string dex_file;
    const LookupTableEntry* entries;
    uint32_t num_entries;
  };

  LookupTables() = default;

  LookupTables(const DexFileListing_079& dex_file_listing,
               const DexFiles& dex_files,
               ConstBuffer oat_buf)
      : oat_buf_(oat_buf) {

    CHECK(dex_file_listing.dex_files().size() == dex_files.headers().size());
    auto listing_it = dex_file_listing.dex_files().begin();
    auto file_it = dex_files.headers().begin();

    tables_.reserve(dex_file_listing.dex_files().size());

    for (unsigned int i = 0; i < dex_file_listing.dex_files().size(); i++) {
      auto table_offset = listing_it->lookup_table_offset;
      const auto& header = *file_it;

      auto num_entries = numEntries(header.class_defs_size);

      auto ptr = oat_buf.slice(table_offset).ptr;

      cur_ma()->markRangeConsumed(ptr, num_entries * sizeof(LookupTableEntry));

      tables_.push_back(LookupTable{
          listing_it->file_offset, listing_it->location,
          reinterpret_cast<const LookupTableEntry*>(ptr), num_entries});

      ++listing_it;
      ++file_it;
    }
  }

  void print() {
    for (const auto& e : tables_) {
      printf(
          "Type_lookup_table[%s]: { \
        num_entries: %u, \
        entries: [",
          e.dex_file.c_str(),
          e.num_entries);
      for (unsigned int i = 0; i < e.num_entries; i++) {
        const auto& entry = e.entries[i];
        if (entry.str_offset != 0) {
          printf(
              "{str: %s, \
            str offset: 0x%08x}",
              oat_buf_.slice(e.dex_file_offset + entry.str_offset).ptr,
              entry.str_offset);
        }
      }
      printf("]}\n");
    }
  }

  static uint32_t numEntries(uint32_t num_classes) {
    return supportedSize(num_classes) ? roundUpToPowerOfTwo(num_classes) : 0u;
  }

  template <typename DexFileType>
  static void write(const std::vector<DexInput>& dex_input_vec,
                    const std::vector<DexFileType>& dex_files,
                    FileHandle& cksum_fh) {
    foreach_pair(
        dex_input_vec,
        dex_files,
        [&](const DexInput& dex_input,
            const DexFileListing_079::DexFile_079& dex_file) {
          CHECK(dex_file.lookup_table_offset == cksum_fh.bytes_written());
          const auto num_classes = dex_file.num_classes;

          const auto lookup_table_size = numEntries(num_classes);
          const auto lookup_table_byte_size =
              lookup_table_size * sizeof(LookupTableEntry);

          auto lookup_table_buf =
              build_lookup_table(dex_input.filename, lookup_table_size);
          auto buf =
              ConstBuffer{reinterpret_cast<const char*>(lookup_table_buf.get()),
                          lookup_table_byte_size};
          write_buf(cksum_fh, buf);
        });
  }

 private:
  static uint32_t hash_str(const std::string& str) {
    uint32_t hash = 0;
    const char* chars = str.c_str();
    while (*chars != '\0') {
      hash = hash * 31 + *chars;
      chars++;
    }
    return hash;
  }

  static uint16_t make_lt_data(uint16_t class_def_idx,
                               uint32_t hash,
                               uint32_t mask) {
    uint16_t hash_mask = static_cast<uint16_t>(~mask);
    return (static_cast<uint16_t>(hash) & hash_mask) | class_def_idx;
  }

  static bool insert_no_probe(LookupTableEntry* table,
                              const LookupTableEntry& entry,
                              uint32_t hash,
                              uint32_t mask) {
    const uint32_t pos = hash & mask;
    if (table[pos].str_offset != 0) {
      return false;
    }
    table[pos] = entry;
    table[pos].next_pos_delta = 0;
    return true;
  }

  static void insert(LookupTableEntry* table,
                     const LookupTableEntry& entry,
                     uint32_t hash,
                     uint32_t mask) {

    // find the last entry in this chain.
    uint32_t pos = hash & mask;
    while (table[pos].next_pos_delta != 0) {
      pos = (pos + table[pos].next_pos_delta) & mask;
    }

    // find the next empty entry
    uint32_t delta = 1;
    while (table[(pos + delta) & mask].str_offset != 0) {
      delta++;
    }
    uint32_t next_pos = (pos + delta) & mask;
    table[pos].next_pos_delta = delta;
    table[next_pos] = entry;
    table[next_pos].next_pos_delta = 0;
  }

  static std::unique_ptr<LookupTableEntry[]> build_lookup_table(
      const std::string& filename, uint32_t lookup_table_size) {

    std::unique_ptr<LookupTableEntry[]> table_buf(
        new LookupTableEntry[lookup_table_size]);
    memset(table_buf.get(), 0, lookup_table_size * sizeof(LookupTableEntry));

    auto dex_fh = FileHandle(fopen(filename.c_str(), "r"));

    DexFileHeader header = {};
    CHECK(dex_fh.fread(&header, sizeof(DexFileHeader), 1) == 1);

    const auto num_classes = header.class_defs_size;
    const auto mask = lookup_table_size - 1;

    // TODO: This is probably the most memory hungry part of the whole building
    // process, but total usage should still be <1MB for all the class strings.
    // if this proves to be a problem we can build the lookup table with redex
    // and ship it to the phone.

    // Read type ids array.
    const auto num_type_ids = header.type_ids_size;
    std::unique_ptr<uint32_t[]> typeid_buf(new uint32_t[num_type_ids]);
    CHECK(dex_fh.seek_set(header.type_ids_off));
    CHECK(dex_fh.fread(typeid_buf.get(), sizeof(uint32_t), num_type_ids) ==
          num_type_ids);

    // Read the string ids array.
    const auto num_string_ids = header.string_ids_size;
    std::unique_ptr<uint32_t[]> stringid_buf(new uint32_t[num_string_ids]);
    CHECK(dex_fh.seek_set(header.string_ids_off));
    CHECK(dex_fh.fread(stringid_buf.get(), sizeof(uint32_t), num_string_ids) ==
          num_string_ids);

    CHECK(dex_fh.seek_set(header.class_defs_off));

    std::unique_ptr<DexClassDef[]> class_defs_buf(new DexClassDef[num_classes]);
    CHECK(dex_fh.fread(class_defs_buf.get(),
                       sizeof(DexClassDef),
                       num_classes) == num_classes);

    constexpr int kClassNameBufSize = 256;

    char class_name_buf[kClassNameBufSize] = {};

    struct Retry {
      uint32_t string_offset;
      uint16_t data;
      uint32_t hash;
    };

    std::vector<Retry> retry_indices;

    for (unsigned int i = 0; i < num_classes; i++) {
      const auto class_idx = class_defs_buf[i].class_idx;
      CHECK(class_idx < num_type_ids);
      const auto string_id = typeid_buf[class_idx];
      CHECK(string_id < num_string_ids);
      const auto string_offset = stringid_buf[string_id];

      dex_fh.seek_set(string_offset);
      auto read_size =
          dex_fh.fread(class_name_buf, sizeof(char), kClassNameBufSize);
      CHECK(read_size > 0);

      auto ptr = class_name_buf;
      const auto str_size = read_uleb128(&ptr) + 1;
      const auto str_start = ptr - class_name_buf;

      std::string class_name;

      if (str_start + str_size >= kClassNameBufSize) {
        std::unique_ptr<char[]> large_class_name_buf(new char[str_size]);
        dex_fh.seek_set(string_offset + str_start);
        CHECK(dex_fh.fread(large_class_name_buf.get(),
                           sizeof(char),
                           str_size) == str_size);
        class_name = std::string(large_class_name_buf.get(), str_size);
      } else {
        class_name = std::string(ptr, str_size);
      }

      const auto hash = hash_str(class_name);
      const auto data = make_lt_data(i, hash, mask);

      if (!insert_no_probe(table_buf.get(),
                           LookupTableEntry{string_offset, data, 0},
                           hash,
                           mask)) {
        retry_indices.emplace_back(Retry{string_offset, data, hash});
      }
    }

    for (const auto& retry : retry_indices) {
      insert(table_buf.get(),
             LookupTableEntry{retry.string_offset, retry.data, 0},
             retry.hash,
             mask);
    }

    return table_buf;
  }

  static bool supportedSize(uint32_t num_class_defs) {
    return num_class_defs != 0u &&
           num_class_defs <= std::numeric_limits<uint16_t>::max();
  }

  ConstBuffer oat_buf_;
  std::vector<LookupTable> tables_;
};

class LookupTables_Nil : public LookupTables {
 public:
  template <typename DexFileListingType>
  static void write(const std::vector<DexInput>&,
                    const DexFileListingType&,
                    FileHandle&) {}
};

// Handles version 064 and 045.
class OatFile_064 : public OatFile {
 public:
  UNCOPYABLE(OatFile_064);
  MOVABLE(OatFile_064);

  static std::unique_ptr<OatFile> parse(bool dex_files_only,
                                        ConstBuffer buf,
                                        size_t oat_offset) {
    auto header = OatHeader::parse(buf);
    auto key_value_store = KeyValueStore(
        buf.slice(header.size()).truncate(header.key_value_store_size));

    auto rest = buf.slice(header.size() + header.key_value_store_size);
    DexFileListing_064 dfl(dex_files_only,
                           static_cast<OatVersion>(header.common.version),
                           header.dex_file_count,
                           rest,
                           buf);

    DexFiles dex_files(dfl, buf);

    return std::unique_ptr<OatFile>(new OatFile_064(header,
                                                    key_value_store,
                                                    std::move(dfl),
                                                    std::move(dex_files),
                                                    oat_offset));
  }

  void print(bool dump_classes,
             bool dump_tables,
             bool print_unverified_classes) override {
    printf("Header:\n");
    header_.print();
    printf("Key/Value store:\n");
    key_value_store_.print();
    printf("Dex File Listing:\n");
    dex_file_listing_.print();
    printf("Dex Files:\n");
    dex_files_.print();
    if (dump_classes) {
      printf("Classes:\n");
      dex_file_listing_.print_classes();
    }
    if (print_unverified_classes) {
      dex_file_listing_.print_unverified_classes();
    }
  }

  Status status() override { return Status::PARSE_SUCCESS; }

  std::vector<OatDexFile> get_oat_dexfiles() override {
    std::vector<OatDexFile> ret;
    ret.reserve(dex_file_listing_.dex_files().size());
    foreach_pair(dex_file_listing_.dex_files(),
                 dex_files_.headers(),
                 [&](const DexFileListing_064::DexFile_064& dex_file,
                     const DexFileHeader& header) {
                   ret.emplace_back(dex_file.location,
                                    dex_file.file_offset,
                                    header.file_size);
                 });
    return ret;
  }

  std::unique_ptr<std::string> get_art_image_loc() const override {
    auto img_loc = key_value_store_.get("image-location");
    if (img_loc == nullptr) {
      return nullptr;
    }
    return std::unique_ptr<std::string>(new std::string(img_loc));
  }

  bool created_by_oatmeal() const override {
    return key_value_store_.has_key(kCreatedByOatmeal);
  }

  std::string version_string() const override {
    char buf[5] = {};
    memcpy(buf, &header_.common.version, 4);
    return std::string(buf);
  }

  size_t oat_offset() const override { return oat_offset_; }

  bool is_samsung() const override { return dex_file_listing_.is_samsung(); }

  static Status build(const std::string& oat_file_name,
                      const std::vector<DexInput>& dex_input,
                      const OatVersion oat_version,
                      InstructionSet isa,
                      bool write_elf,
                      const std::string& art_image_location,
                      bool samsung_mode,
                      const QuickData* quick_data);

 private:
  OatFile_064(OatHeader h,
              KeyValueStore kv,
              DexFileListing_064 dfl,
              DexFiles dex_files,
              size_t oat_data_offset)
      : header_(h),
        key_value_store_(std::move(kv)),
        dex_file_listing_(std::move(dfl)),
        dex_files_(std::move(dex_files)),
        oat_offset_(oat_data_offset) {}

  OatHeader header_;
  KeyValueStore key_value_store_;
  DexFileListing_064 dex_file_listing_;
  DexFiles dex_files_;
  size_t oat_offset_;
};

// OatFile format for 079 and 088. (088 may have changes that don't
// show up with verify-none. So far it appears to be identical.)
class OatFile_079 : public OatFile {
 public:
  UNCOPYABLE(OatFile_079);
  MOVABLE(OatFile_079);

  static std::unique_ptr<OatFile> parse(bool dex_files_only,
                                        ConstBuffer buf,
                                        size_t oat_offset) {
    auto header = OatHeader::parse(buf);
    auto key_value_store = KeyValueStore(
        buf.slice(header.size()).truncate(header.key_value_store_size));
    auto rest = buf.slice(header.size() + header.key_value_store_size);

    DexFileListing_079 dfl(header.dex_file_count, rest);

    DexFiles dex_files(dfl, buf);

    if (dex_files_only) {
      return std::unique_ptr<OatFile>(new OatFile_079(header,
                                                      key_value_store,
                                                      std::move(dfl),
                                                      std::move(dex_files),
                                                      oat_offset));
    }

    LookupTables lookup_tables(dfl, dex_files, buf);

    OatClasses_079 oat_classes(dfl, dex_files, buf);

    return std::unique_ptr<OatFile>(new OatFile_079(header,
                                                    key_value_store,
                                                    std::move(dfl),
                                                    std::move(dex_files),
                                                    std::move(lookup_tables),
                                                    std::move(oat_classes),
                                                    oat_offset));
  }

  void print(bool dump_classes,
             bool dump_tables,
             bool print_unverified_classes) override {
    printf("Header:\n");
    header_.print();
    printf("Key/Value store:\n");
    key_value_store_.print();
    printf("Dex File Listing:\n");
    dex_file_listing_.print();
    printf("Dex Files:\n");
    dex_files_.print();

    if (dump_tables) {
      printf("LookupTables:\n");
      lookup_tables_.print();
    }
    if (dump_classes) {
      printf("Classes:\n");
      oat_classes_.print();
    }
    if (print_unverified_classes) {
      oat_classes_.print_unverified_classes();
    }
  }

  Status status() override { return Status::PARSE_SUCCESS; }

  static Status build(const std::string& oat_file_name,
                      const std::vector<DexInput>& dex_input,
                      const OatVersion oat_version,
                      InstructionSet isa,
                      bool write_elf,
                      const std::string& art_image_location,
                      bool samsung_mode,
                      const QuickData* quick_data);

  std::vector<OatDexFile> get_oat_dexfiles() override {
    std::vector<OatDexFile> ret;
    ret.reserve(dex_file_listing_.dex_files().size());
    for (const auto& dex : dex_file_listing_.dex_files()) {
      ret.emplace_back(dex.location, dex.location_checksum, dex.file_offset);
    }
    return ret;
  }

  size_t oat_offset() const override { return oat_offset_; }

  // Samsung has no custom modifications (that i know of) on 079 and up, so
  // there's nothing to detect.
  bool is_samsung() const override { return false; }

  std::unique_ptr<std::string> get_art_image_loc() const override {
    auto img_loc = key_value_store_.get("image-location");
    if (img_loc == nullptr) {
      return nullptr;
    }
    return std::unique_ptr<std::string>(new std::string(img_loc));
  }

  bool created_by_oatmeal() const override {
    return key_value_store_.has_key(kCreatedByOatmeal);
  }

  std::string version_string() const override {
    char buf[5] = {};
    memcpy(buf, &header_.common.version, 4);
    return std::string(buf);
  }

 private:
  OatFile_079(OatHeader h,
              KeyValueStore kv,
              DexFileListing_079 dfl,
              DexFiles dex_files,
              size_t oat_data_offset)
      : header_(h),
        key_value_store_(std::move(kv)),
        dex_file_listing_(std::move(dfl)),
        dex_files_(std::move(dex_files)),
        oat_offset_(oat_data_offset) {}

  OatFile_079(OatHeader h,
              KeyValueStore kv,
              DexFileListing_079 dfl,
              DexFiles dex_files,
              LookupTables lt,
              OatClasses_079 oat_classes,
              size_t oat_data_offset)
      : header_(h),
        key_value_store_(std::move(kv)),
        dex_file_listing_(std::move(dfl)),
        dex_files_(std::move(dex_files)),
        lookup_tables_(std::move(lt)),
        oat_classes_(std::move(oat_classes)),
        oat_offset_(oat_data_offset) {}

  OatHeader header_;
  KeyValueStore key_value_store_;
  DexFileListing_079 dex_file_listing_;
  DexFiles dex_files_;
  LookupTables lookup_tables_;
  OatClasses_079 oat_classes_;
  size_t oat_offset_;
};

// OatFile format for 124/V131
// Key difference is the parsing of OAT and DEX has to be done in different
// files instead on a single everything.oat file.
class OatFile_124 : public OatFile {
 public:
  UNCOPYABLE(OatFile_124);
  MOVABLE(OatFile_124);

  template <typename DexFileListingType, typename OatFileType>
  static std::unique_ptr<OatFile> oatfile_124_131_parse(
      bool dex_files_only,
      ConstBuffer buf,
      size_t oat_offset,
      const std::vector<DexInput>& dexes) {
    if (dexes.size() != 1) {
      fprintf(stderr,
              "V124/V131 odex files must come accompained with one and only "
              "one vdex file\n");
      return nullptr;
    }

    auto header = OatHeader::parse(buf);
    auto key_value_store = KeyValueStore(
        buf.slice(header.size()).truncate(header.key_value_store_size));

    auto rest = buf.slice(header.size() + header.key_value_store_size);
    DexFileListingType dfl(header.dex_file_count, rest);

    auto dex_file_name = dexes[0].filename;
    auto dex_file = FileHandle(fopen(dex_file_name.c_str(), "r"));
    if (dex_file.get() == nullptr) {
      fprintf(stderr,
              "failed to open dex file %s %s\n",
              dex_file_name.c_str(),
              std::strerror(errno));
      return nullptr;
    }

    auto dex_file_size = get_filesize(dex_file);

    // We don't run dumping during install on device, so it is allowed to
    // consume lots of memory.
    auto dex_file_contents = std::make_unique<char[]>(dex_file_size);
    auto dexFileBytesRead =
        fread(dex_file_contents.get(), 1, dex_file_size, dex_file.get());
    if (dexFileBytesRead != dex_file_size) {
      fprintf(stderr,
              "Failed to read dex file %s (%zd)\n",
              std::strerror(errno),
              dexFileBytesRead);
      return nullptr;
    }

    ConstBuffer dex_file_buf{dex_file_contents.get(), dex_file_size};
    cur_ma()->addBuffer(dex_file_buf);
    DexFiles dex_files(dfl, dex_file_buf);

    if (dex_files_only) {
      return std::unique_ptr<OatFile>(new OatFileType(header,
                                                      key_value_store,
                                                      std::move(dfl),
                                                      std::move(dex_files),
                                                      oat_offset));
    }

    LookupTables lookup_tables(dfl, dex_files, buf);
    OatClasses_124 oat_classes(dfl, dex_files, buf, dex_file_buf);

    return std::unique_ptr<OatFile>(new OatFileType(header,
                                                    key_value_store,
                                                    std::move(dfl),
                                                    std::move(dex_files),
                                                    std::move(lookup_tables),
                                                    std::move(oat_classes),
                                                    oat_offset));
  }

  static std::unique_ptr<OatFile> parse(bool dex_files_only,
                                        ConstBuffer buf,
                                        size_t oat_offset,
                                        const std::vector<DexInput>& dexes) {

    return oatfile_124_131_parse<DexFileListing_124, OatFile_124>(
        dex_files_only, buf, oat_offset, dexes);
  }

  void print(bool dump_classes,
             bool dump_tables,
             bool print_unverified_classes) override {
    printf("Header:\n");
    header_.print();
    printf("Key/Value store:\n");
    key_value_store_.print();
    printf("Dex File Listing:\n");
    dex_file_listing_->print();
    printf("Dex Files:\n");
    dex_files_.print();

    if (dump_tables) {
      printf("LookupTables:\n");
      lookup_tables_.print();
    }
    if (dump_classes) {
      printf("Classes:\n");
      oat_classes_.print();
    }
    if (print_unverified_classes) {
      oat_classes_.print_unverified_classes();
    }
  }

  bool is_samsung() const override { return false; }

  Status status() override { return Status::PARSE_SUCCESS; }

  static Status build(const std::string& oat_file_name,
                      const std::vector<DexInput>& dex_input,
                      const OatVersion oat_version,
                      InstructionSet isa,
                      bool write_elf,
                      const std::string& art_image_location,
                      bool samsung_mode,
                      const QuickData* quick_data);

  std::vector<OatDexFile> get_oat_dexfiles() override {
    std::vector<OatDexFile> ret;
    ret.reserve(dex_file_listing_->dex_files().size());
    for (const auto& dex : dex_file_listing_->dex_files()) {
      ret.emplace_back(dex.location, dex.location_checksum, dex.file_offset);
    }
    return ret;
  }

  size_t oat_offset() const override { return oat_offset_; }

  std::unique_ptr<std::string> get_art_image_loc() const override {
    auto img_loc = key_value_store_.get("image-location");
    if (img_loc == nullptr) {
      return nullptr;
    }
    return std::unique_ptr<std::string>(new std::string(img_loc));
  }

  bool created_by_oatmeal() const override {
    return key_value_store_.has_key(kCreatedByOatmeal);
  }

  std::string version_string() const override {
    char buf[5] = {};
    memcpy(buf, &header_.common.version, 4);
    return std::string(buf);
  }

 private:
  OatFile_124(OatHeader h,
              KeyValueStore kv,
              DexFileListing_124 dfl,
              DexFiles dex_files,
              size_t oat_data_offset)
      : header_(h),
        key_value_store_(std::move(kv)),
        dex_files_(std::move(dex_files)),
        oat_offset_(oat_data_offset),
        dex_file_listing_(&dfl) {}

  OatFile_124(OatHeader h,
              KeyValueStore kv,
              DexFileListing_124 dfl,
              DexFiles dex_files,
              LookupTables lt,
              OatClasses_124 oat_classes,
              size_t oat_data_offset)
      : header_(h),
        key_value_store_(std::move(kv)),
        dex_files_(std::move(dex_files)),
        lookup_tables_(std::move(lt)),
        oat_classes_(std::move(oat_classes)),
        oat_offset_(oat_data_offset),
        dex_file_listing_(&dfl) {}

 protected:
  OatFile_124(OatHeader h,
              KeyValueStore kv,
              DexFiles dex_files,
              size_t oat_data_offset)
      : header_(h),
        key_value_store_(std::move(kv)),
        dex_files_(std::move(dex_files)),
        oat_offset_(oat_data_offset),
        dex_file_listing_(nullptr) {}

  OatFile_124(OatHeader h,
              KeyValueStore kv,
              DexFiles dex_files,
              LookupTables lt,
              OatClasses_124 oat_classes,
              size_t oat_data_offset)
      : header_(h),
        key_value_store_(std::move(kv)),
        dex_files_(std::move(dex_files)),
        lookup_tables_(std::move(lt)),
        oat_classes_(std::move(oat_classes)),
        oat_offset_(oat_data_offset),
        dex_file_listing_(nullptr) {}

  OatHeader header_;
  KeyValueStore key_value_store_;
  DexFiles dex_files_;
  LookupTables lookup_tables_;
  OatClasses_124 oat_classes_;
  size_t oat_offset_;

 private:
  std::unique_ptr<DexFileListing_124> dex_file_listing_;
};

class OatFile_131 : public OatFile_124 {
 public:
  UNCOPYABLE(OatFile_131);
  MOVABLE(OatFile_131);

  void print(bool dump_classes,
             bool dump_tables,
             bool print_unverified_classes) override {
    printf("Header:\n");
    header_.print();
    printf("Key/Value store:\n");
    key_value_store_.print();
    printf("Dex File Listing:\n");
    dex_file_listing_.print();
    printf("Dex Files:\n");
    dex_files_.print();

    if (dump_tables) {
      printf("LookupTables:\n");
      lookup_tables_.print();
    }
    if (dump_classes) {
      printf("Classes:\n");
      oat_classes_.print();
    }
    if (print_unverified_classes) {
      oat_classes_.print_unverified_classes();
    }
  }

  bool is_samsung() const override { return false; }

  Status status() override { return Status::PARSE_SUCCESS; }

  static Status build(const std::string& oat_file_name,
                      const std::vector<DexInput>& dex_input,
                      const OatVersion oat_version,
                      InstructionSet isa,
                      bool write_elf,
                      const std::string& art_image_location,
                      bool samsung_mode,
                      const QuickData* quick_data);

  std::vector<OatDexFile> get_oat_dexfiles() override {
    std::vector<OatDexFile> ret;
    ret.reserve(dex_file_listing_.dex_files().size());
    for (const auto& dex : dex_file_listing_.dex_files()) {
      ret.emplace_back(dex.location, dex.location_checksum, dex.file_offset);
    }
    return ret;
  }

  size_t oat_offset() const override { return oat_offset_; }

  std::unique_ptr<std::string> get_art_image_loc() const override {
    auto img_loc = key_value_store_.get("image-location");
    if (img_loc == nullptr) {
      return nullptr;
    }
    return std::unique_ptr<std::string>(new std::string(img_loc));
  }

  bool created_by_oatmeal() const override {
    return key_value_store_.has_key(kCreatedByOatmeal);
  }

  std::string version_string() const override {
    char buf[5] = {};
    memcpy(buf, &header_.common.version, 4);
    return std::string(buf);
  }

 private:
  OatFile_131(OatHeader h,
              KeyValueStore kv,
              DexFileListing_131 dfl,
              DexFiles dex_files,
              size_t oat_data_offset)
      : OatFile_124(h, std::move(kv), std::move(dex_files), oat_data_offset),
        dex_file_listing_(std::move(dfl)) {}

  OatFile_131(OatHeader h,
              KeyValueStore kv,
              DexFileListing_131 dfl,
              DexFiles dex_files,
              LookupTables lt,
              OatClasses_124 oat_classes,
              size_t oat_data_offset)
      : OatFile_124(h,
                    std::move(kv),
                    std::move(dex_files),
                    std::move(lt),
                    std::move(oat_classes),
                    oat_data_offset),
        dex_file_listing_(std::move(dfl)) {}

  DexFileListing_131 dex_file_listing_;
};

class OatFile_Unknown : public OatFile {
 public:
  void print(bool dump_classes,
             bool dump_tables,
             bool print_unverified_classes) override {
    printf("Unknown OAT file version!\n");
    header_.print();
  }

  Status status() override { return Status::PARSE_UNKNOWN_VERSION; }

  size_t oat_offset() const override { return 0; }

  static std::unique_ptr<OatFile> parse(ConstBuffer buf) {
    return std::unique_ptr<OatFile>(new OatFile_Unknown(buf));
  }

  std::vector<OatDexFile> get_oat_dexfiles() override {
    return std::vector<OatDexFile>();
  }

  std::unique_ptr<std::string> get_art_image_loc() const override {
    return nullptr;
  }

  bool created_by_oatmeal() const override { return false; }

  std::string version_string() const override { return ""; }

  bool is_samsung() const override { return false; }

 private:
  explicit OatFile_Unknown(ConstBuffer buf) {
    header_ = OatHeader_Common::parse(buf);
  }

  OatHeader_Common header_;
};

class OatFile_Bad : public OatFile {
 public:
  void print(bool dump_classes,
             bool dump_tables,
             bool print_unverified_classes) override {
    printf("Bad magic number:\n");
    header_.print();
  }

  Status status() override { return Status::PARSE_BAD_MAGIC_NUMBER; }

  size_t oat_offset() const override { return 0; }

  static std::unique_ptr<OatFile> parse(ConstBuffer buf) {
    return std::unique_ptr<OatFile>(new OatFile_Bad(buf));
  }

  std::vector<OatDexFile> get_oat_dexfiles() override {
    return std::vector<OatDexFile>();
  }

  std::unique_ptr<std::string> get_art_image_loc() const override {
    return nullptr;
  }

  bool created_by_oatmeal() const override { return false; }

  std::string version_string() const override { return ""; }

  bool is_samsung() const override { return false; }

 private:
  explicit OatFile_Bad(ConstBuffer buf) {
    header_ = OatHeader_Common::parse(buf);
  }

  OatHeader_Common header_;
};

} // namespace

OatFile::~OatFile() = default;

static std::unique_ptr<OatFile> parse_oatfile_impl(
    bool dex_files_only,
    ConstBuffer oatfile_buffer,
    const std::vector<DexInput>& dexes) {
  constexpr size_t kOatElfOffset = 0x1000;

  size_t oat_offset = 0;
  if (oatfile_buffer.len >= std::max(kOatElfOffset, sizeof(Elf32_Ehdr))) {
    Elf32_Ehdr header;
    memcpy(&header, oatfile_buffer.ptr, sizeof(Elf32_Ehdr));
    if (header.e_ident[0] == 0x7f && header.e_ident[1] == 'E' &&
        header.e_ident[2] == 'L' && header.e_ident[3] == 'F') {
      // .rodata starts at 0x1000 in every version of ART that i've seen.
      // If there are any where this isn't true, we'll have to actually read
      // out the offset of .rodata.
      oat_offset = 0x1000;
      oatfile_buffer = oatfile_buffer.slice(0x1000);
    }
  }

  if (oatfile_buffer.len < sizeof(OatHeader)) {
    return nullptr;
  }

  auto header = OatHeader_Common::parse(oatfile_buffer);

  // TODO: do we need to handle endian-ness? I think all platforms we
  // care about are little-endian.
  if (header.magic != kOatMagicNum) {
    return OatFile_Bad::parse(oatfile_buffer);
  }

  switch (static_cast<OatVersion>(header.version)) {
  case OatVersion::V_039:
  case OatVersion::V_045:
  case OatVersion::V_064:
  case OatVersion::V_067:
    return OatFile_064::parse(dex_files_only, oatfile_buffer, oat_offset);
  case OatVersion::V_079:
  case OatVersion::V_088:
    // 079 and 088 are the same as far as I can tell.
    return OatFile_079::parse(dex_files_only, oatfile_buffer, oat_offset);
  case OatVersion::V_124:
  case OatVersion::V_131:
    return OatFile_124::parse(
        dex_files_only, oatfile_buffer, oat_offset, dexes);
  case OatVersion::UNKNOWN:
    return OatFile_Unknown::parse(oatfile_buffer);
  }
  fprintf(stderr, "Unhandled oat version 0x%08x\n", header.version);
  return std::unique_ptr<OatFile>(nullptr);
}

std::unique_ptr<OatFile> OatFile::parse(ConstBuffer oatfile_buffer,
                                        const std::vector<DexInput>& dex_files,
                                        bool dex_files_only) {
  return parse_oatfile_impl(dex_files_only, oatfile_buffer, dex_files);
}

std::unique_ptr<OatFile> OatFile::parse_dex_files_only(ConstBuffer buf) {
  return parse_oatfile_impl(true, buf, std::vector<DexInput>());
}

std::unique_ptr<OatFile> OatFile::parse_dex_files_only(void* ptr, size_t len) {
  return parse_dex_files_only(ConstBuffer{reinterpret_cast<char*>(ptr), len});
}

////////// building

OatHeader build_header(OatVersion oat_version,
                       const std::vector<DexInput>& dex_input,
                       InstructionSet isa,
                       uint32_t keyvalue_size,
                       uint32_t oat_size,
                       const ImageInfo_064* image_info) {
  OatHeader header;

  header.common.magic = kOatMagicNum;
  header.common.version = static_cast<uint32_t>(oat_version);

  // the checksum must be re-written after we've written
  // the rest of the file, as we don't know the checksum until then.
  header.common.adler32_checksum = 0xcdcdcdcd;

  header.instruction_set = isa;

  // This appears to be set to 1 on both x86 and arm, not clear if there is
  // ever a case where we need to parameterize this.
  header.instruction_set_features_bitmap = 1;

  header.dex_file_count = dex_input.size();

  header.executable_offset = oat_size;

  header.key_value_store_size = keyvalue_size;

  if (image_info != nullptr) {
    header.image_patch_delta = image_info->patch_delta;
    header.image_file_location_oat_checksum = image_info->oat_checksum;
    header.image_file_location_oat_data_begin = image_info->data_begin;
  }

  // omitted fields default to zero, and should remain zero.
  return header;
}

std::vector<DexFileListing_064::DexFile_064> DexFileListing_064::build(
    const std::vector<DexInput>& dex_input,
    uint32_t& next_offset,
    bool samsung_mode) {
  // next_offset points to the first byte after the DexFileListing
  CHECK(is_aligned<4>(next_offset));

  uint32_t total_lookup_table_size = 0;
  uint32_t total_dex_size = 0;
  uint32_t total_class_info_size = 0;

  std::vector<DexFileListing_064::DexFile_064> dex_files;
  dex_files.reserve(dex_input.size());

  for (const auto& dex : dex_input) {
    auto dex_offset = next_offset + total_dex_size;

    auto dex_fh = FileHandle(fopen(dex.filename.c_str(), "r"));
    CHECK(dex_fh.get() != nullptr);

    auto file_size = get_filesize(dex_fh);

    // dex files are 4-byte aligned inside the oatfile.
    auto padded_size = align<4>(file_size);

    CHECK(file_size >= sizeof(DexFileHeader));

    // read the header to get the count of classes.
    DexFileHeader header = {};
    CHECK(dex_fh.fread(&header, sizeof(DexFileHeader), 1) == 1);

    const auto num_classes = header.class_defs_size;
    const auto num_types = header.type_ids_size;

    total_class_info_size += num_classes * sizeof(uint32_t);
    total_dex_size += padded_size;

    if (samsung_mode) {
      total_lookup_table_size += SamsungLookupTables::rawSize(num_types);
    }

    std::vector<ClassInfo> classes;
    for (unsigned int i = 0; i < num_classes; i++) {
      classes.push_back(ClassInfo(OatClasses::Status::kStatusVerified,
                                  OatClasses::Type::kOatClassNoneCompiled));
    }

    dex_files.push_back(DexFileListing_064::DexFile_064(
        dex.location,
        header.checksum,
        dex_offset,
        // temporarily store a count, will be translated to an offset after this
        // loop.
        num_types,
        std::vector<uint32_t>(num_classes),
        classes));
  }

  if (samsung_mode) {
    // need to adjust all dex offsets forward by the total lookup table size.
    for (auto& dex : dex_files) {
      dex.file_offset += total_lookup_table_size;
    }
    CHECK(is_aligned<4>(next_offset));

    // adjust the lookup_table offsets for each dex. lookup_table_offset current
    // stores the number of types.
    for (auto& dex : dex_files) {
      const auto num_types = dex.lookup_table_offset;
      dex.lookup_table_offset = next_offset;
      const auto raw_size = SamsungLookupTables::rawSize(num_types);
      next_offset += raw_size;
    }
  }

  CHECK(is_aligned<4>(next_offset));

  next_offset += total_dex_size;
  auto first_class_info_offset = next_offset;
  next_offset += total_class_info_size;

  // Adjust the class offset tables for each dex, now that we have accounted
  // for the dex size.
  for (auto& dex : dex_files) {
    for (auto& offset : dex.class_offsets) {
      offset = first_class_info_offset;
      first_class_info_offset += sizeof(ClassInfo);
    }
  }

  return dex_files;
}

std::vector<DexFileListing_079::DexFile_079> DexFileListing_079::build(
    const std::vector<DexInput>& dex_input,
    uint32_t& next_offset,
    bool /*samsung_mode*/) {
  uint32_t total_dex_size = 0;

  std::vector<DexFileListing_079::DexFile_079> dex_files;
  dex_files.reserve(dex_input.size());

  for (const auto& dex : dex_input) {
    auto dex_offset = next_offset + total_dex_size;

    auto dex_fh = FileHandle(fopen(dex.filename.c_str(), "r"));
    CHECK(dex_fh.get() != nullptr);

    auto file_size = get_filesize(dex_fh);

    // dex files are 4-byte aligned inside the oatfile.
    auto padded_size = align<4>(file_size);
    total_dex_size += padded_size;

    CHECK(file_size >= sizeof(DexFileHeader));

    // read the header to get the count of classes.
    DexFileHeader header = {};
    CHECK(dex_fh.fread(&header, sizeof(DexFileHeader), 1) == 1);

    auto num_classes = header.class_defs_size;
    auto class_table_size =
        num_classes * sizeof(uint32_t) // array of pointers to ClassInfo structs
        + num_classes * sizeof(OatClasses::ClassInfo);

    auto lookup_table_size = LookupTables::numEntries(num_classes) *
                             sizeof(LookupTables::LookupTableEntry);

    dex_files.push_back(DexFileListing_079::DexFile_079(
        dex.location,
        header.checksum,
        dex_offset,
        // temporarily store sizes instead of offsets here.
        // they will be replaced with offsets in the next loop.
        num_classes,
        class_table_size,
        lookup_table_size));
  }
  next_offset += total_dex_size;

  CHECK(is_aligned<4>(next_offset));
  // note non-const ref
  for (auto& dex_file : dex_files) {
    auto cur_size = dex_file.classes_offset;
    dex_file.classes_offset = next_offset;
    next_offset += cur_size;
    CHECK(is_aligned<4>(next_offset));
  }
  for (auto& dex_file : dex_files) {
    auto cur_size = dex_file.lookup_table_offset;
    dex_file.lookup_table_offset = next_offset;
    next_offset += cur_size;
    CHECK(is_aligned<4>(next_offset));
  }

  return dex_files;
}

std::vector<DexFileListing_124::DexFile_124> DexFileListing_124::build(
    const std::vector<DexInput>& dex_input,
    uint32_t& next_offset,
    bool /*samsung_mode*/) {
  CHECK(dex_input.size() == 1);

  std::vector<DexFileListing_124::DexFile_124> dex_files;
  dex_files.reserve(dex_input.size());

  for (const auto& dex : dex_input) {
    // We load the dex bytecode in the VDEX file after the header and
    // the checksum for the DEX right after.
    auto dex_offset = sizeof(VdexFileHeader) + sizeof(uint32_t);

    auto dex_fh = FileHandle(fopen(dex.filename.c_str(), "r"));
    CHECK(dex_fh.get() != nullptr);

    auto file_size = get_filesize(dex_fh);
    CHECK(file_size >= sizeof(DexFileHeader));

    // read the header to get the count of classes.
    DexFileHeader header = {};
    CHECK(dex_fh.fread(&header, sizeof(DexFileHeader), 1) == 1);

    auto num_classes = header.class_defs_size;
    auto class_table_size =
        num_classes * sizeof(uint32_t) // array of pointers to ClassInfo structs
        + num_classes * sizeof(OatClasses::ClassInfo);

    auto lookup_table_size = LookupTables::numEntries(num_classes) *
                             sizeof(LookupTables::LookupTableEntry);

    dex_files.push_back(DexFileListing_124::DexFile_124(
        dex.location,
        header.checksum,
        dex_offset,
        // temporarily store sizes instead of offsets here.
        // they will be replaced with offsets in the next loop.
        num_classes,
        class_table_size,
        lookup_table_size));
  }

  CHECK(is_aligned<4>(next_offset));
  // note non-const ref
  for (auto& dex_file : dex_files) {
    auto cur_size = dex_file.classes_offset;
    dex_file.classes_offset = next_offset;
    next_offset += cur_size;
    CHECK(is_aligned<4>(next_offset));
  }
  for (auto& dex_file : dex_files) {
    auto cur_size = dex_file.lookup_table_offset;
    dex_file.lookup_table_offset = next_offset;
    next_offset += cur_size;
    CHECK(is_aligned<4>(next_offset));
  }

  return dex_files;
}

std::vector<DexFileListing_131::DexFile_131> DexFileListing_131::build(
    const std::vector<DexInput>& dex_input,
    uint32_t& next_offset,
    bool /*samsung_mode*/) {
  CHECK(dex_input.size() == 1);

  std::vector<DexFileListing_131::DexFile_131> dex_files;
  dex_files.reserve(dex_input.size());

  for (const auto& dex : dex_input) {
    // We load the dex bytecode in the VDEX file after the header and
    // the checksum for the DEX right after.
    auto dex_offset = sizeof(VdexFileHeader) + sizeof(uint32_t);

    auto dex_fh = FileHandle(fopen(dex.filename.c_str(), "r"));
    CHECK(dex_fh.get() != nullptr);

    auto file_size = get_filesize(dex_fh);
    CHECK(file_size >= sizeof(DexFileHeader));

    // read the header to get the count of classes.
    DexFileHeader header = {};
    CHECK(dex_fh.fread(&header, sizeof(DexFileHeader), 1) == 1);

    auto num_classes = header.class_defs_size;
    auto class_table_size =
        num_classes * sizeof(uint32_t) // array of pointers to ClassInfo structs
        + num_classes * sizeof(OatClasses::ClassInfo);

    auto lookup_table_size = LookupTables::numEntries(num_classes) *
                             sizeof(LookupTables::LookupTableEntry);

    dex_files.push_back(DexFileListing_131::DexFile_131(
        dex.location,
        header.checksum,
        dex_offset,
        // temporarily store sizes instead of offsets here.
        // they will be replaced with offsets in the next loop.
        num_classes,
        class_table_size,
        lookup_table_size,
        0, // dex_layout_sections_offset
        0)); // method_bss_mapping_offset
  }

  CHECK(is_aligned<4>(next_offset));
  // note non-const ref
  for (auto& dex_file : dex_files) {
    auto cur_size = dex_file.classes_offset;
    dex_file.classes_offset = next_offset;
    next_offset += cur_size;
    CHECK(is_aligned<4>(next_offset));
  }
  for (auto& dex_file : dex_files) {
    auto cur_size = dex_file.lookup_table_offset;
    dex_file.lookup_table_offset = next_offset;
    next_offset += cur_size;
    CHECK(is_aligned<4>(next_offset));
  }

  return dex_files;
}

void write_dex_file(const DexInput& input,
                    const QuickData* quick_data,
                    FileHandle& cksum_fh) {
  if (quick_data != nullptr) {
    START_TRACE()
    quicken_dex(input.filename.c_str(), quick_data, cksum_fh);
    END_TRACE("quicken_dex")
  } else {
    START_TRACE()
    auto dex_fh = FileHandle(fopen(input.filename.c_str(), "r"));
    stream_file(dex_fh, cksum_fh);
    END_TRACE("stream_dex")
  }
}

template <typename DexFileListingType>
void write_dex_files(const std::vector<DexInput>& dex_input,
                     const std::vector<DexFileListingType>& dex_files,
                     const QuickData* quick_data,
                     FileHandle& cksum_fh) {
  foreach_pair(dex_input,
               dex_files,
               [&](const DexInput& input, const DexFileListingType& dex_file) {
                 CHECK(dex_file.file_offset == cksum_fh.bytes_written());
                 write_dex_file(input, quick_data, cksum_fh);
               });
}

// We only ship to 32 bit platforms so this is always 4.
static constexpr size_t pointer_size = 4;

static size_t types_size(size_t num_elements) {
  return std::max(num_elements * pointer_size, pointer_size);
}

static size_t methods_size(size_t num_elements) {
  return std::max(pointer_size * num_elements, pointer_size);
}

static size_t strings_size(size_t num_elements) {
  return pointer_size * num_elements;
}

static size_t fields_size(size_t num_elements) {
  return pointer_size * num_elements;
}

static size_t compute_bss_size_079(const std::vector<DexInput>& dex_files) {
  size_t ret = 0;

  for (const auto& e : dex_files) {
    auto dex_fh = FileHandle(fopen(e.filename.c_str(), "r"));
    DexFileHeader header = {};
    CHECK(dex_fh.fread(&header, sizeof(DexFileHeader), 1) == 1);

    auto meth_offset = align<pointer_size>(types_size(header.type_ids_size));
    auto strings_offset =
        align<pointer_size>(meth_offset + methods_size(header.method_ids_size));
    auto fields_offset = align<pointer_size>(
        strings_offset + strings_size(header.string_ids_size));
    auto size =
        align<pointer_size>(fields_offset + fields_size(header.field_ids_size));
    ret += size;
  }
  return ret;
}

std::unique_ptr<ImageInfo_064> read_image_info_064(
    const std::string& art_image_location) {
  auto art_fh = FileHandle(fopen(art_image_location.c_str(), "r"));
  if (art_fh.get() == nullptr) {
    return std::unique_ptr<ImageInfo_064>(nullptr);
  }

  auto art_header = ArtImageHeader::parse(art_fh);
  if (!art_header) {
    return std::unique_ptr<ImageInfo_064>(nullptr);
  }

  return std::unique_ptr<ImageInfo_064>(
      new ImageInfo_064(art_header->patch_delta,
                        art_header->oat_checksum,
                        art_header->oat_data_begin));
}

std::unique_ptr<QuickData> read_quick_data(
    const std::string& quick_data_location) {
  struct stat buffer;
  if (stat(quick_data_location.c_str(), &buffer) != 0) {
    fprintf(stderr,
            "Failed to locate quickening metadata file: %s\n",
            quick_data_location.c_str());
    return std::unique_ptr<QuickData>(nullptr);
  }
  return std::make_unique<QuickData>(quick_data_location.c_str());
}

template <typename DexFileListingType,
          typename OatClassesType,
          typename LookupTablesType,
          typename SamsungLookupTablesType>
OatFile::Status build_oatfile(const std::string& oat_file_name,
                              const std::vector<DexInput>& dex_input,
                              const OatVersion oat_version,
                              InstructionSet isa,
                              bool write_elf,
                              const std::string& art_image_location,
                              bool samsung_mode,
                              const QuickData* quick_data) {

  const std::vector<KeyValueStore::KeyValue> key_value = {
      {"classpath", ""},
      {"compiler-filter", "verify-none"},
      {"debuggable", "false"},
      // What ever will happen if art tries to use this?
      {"dex2oat-cmdline", "--oat-file=/dev/null --dex-file=/dev/null"},
      {"dex2oat-host", "X86"},
      {"has-patch-info", "false"},
      {"native-debuggable", "false"},
      {"image-location", art_image_location.c_str()},
      {"pic", "false"},
      {kCreatedByOatmeal, "true"}};

  ////////// Gather image info from boot.art and boot.oat
  std::unique_ptr<ImageInfo_064> image_info;
  if (oat_version == OatVersion::V_067 || oat_version == OatVersion::V_064 ||
      oat_version == OatVersion::V_045 || oat_version == OatVersion::V_039) {
    image_info = read_image_info_064(art_image_location);
  }

  ////////// Compute sizes and offsets.

  const auto keyvalue_size = KeyValueStore::compute_size(key_value);
  const auto dex_file_listing_size =
      DexFileListingType::compute_size(dex_input, samsung_mode);

  // Neither the keyvalue store or the DexFileListing require alignment.
  uint32_t next_offset = align<4>(OatHeader::size(oat_version) + keyvalue_size +
                                  dex_file_listing_size);

  // next_offset points to end of last dexfile listing.
  auto dex_files =
      DexFileListingType::build(dex_input, next_offset, samsung_mode);

  auto oat_size = align<0x1000>(next_offset);

  auto header = build_header(
      oat_version, dex_input, isa, keyvalue_size, oat_size, image_info.get());

  ////////// Write the file.

  auto oat_fh = FileHandle(fopen(oat_file_name.c_str(), "w"));
  if (oat_fh.get() == nullptr) {
    return OatFile::Status::BUILD_IO_ERROR;
  }

  if (write_elf) {
    write_padding(oat_fh, 0, 0x1000);
    oat_fh.set_seek_reference_to_fpos();
    oat_fh.reset_bytes_written();
  }

  header.write(oat_fh);

  // Write key value store.
  KeyValueStore::write(oat_fh, key_value);

  // write DexFileListing
  DexFileListingType::write(oat_fh, dex_files, samsung_mode);

  // Write padding to align to 4 bytes.
  auto padding = align<4>(oat_fh.bytes_written()) - oat_fh.bytes_written();
  char buf[4] = {};
  write_buf(oat_fh, ConstBuffer{buf, padding});

  // Write lookup tables.
  if (samsung_mode) {
    SamsungLookupTablesType::write(dex_input, dex_files, oat_fh);
  }

  write_dex_files(dex_input, dex_files, quick_data, oat_fh);
  OatClassesType::write(dex_files, oat_fh);

  LookupTablesType::write(dex_input, dex_files, oat_fh);

  // Pad with 0s up to oat_size
  write_padding(oat_fh, 0, oat_size - oat_fh.bytes_written());

  ////////// Update header with final checksum.

  CHECK(oat_fh.seek_begin());

  // Note: So far, I can't replicate the checksum computation done by
  // dex2oat. It appears that the file is written in a fairly arbitrary
  // order, and the checksum is computed as those sections are written.
  // Fortunately, art does not seem to verify the checksum at any point.
  // We don't even attempt to compute the checksum now, as it takes a few
  // seconds to do so.
  header.common.adler32_checksum = 0xcdcdcdcd;

  write_obj(oat_fh, header.common);

  if (write_elf) {
    oat_fh.set_seek_reference(0);
    oat_fh.seek_begin();

    ElfWriter section_headers(oat_version);
    section_headers.build(isa, oat_size, compute_bss_size_079(dex_input));
    section_headers.write(oat_fh);
  }

  return OatFile::Status::BUILD_SUCCESS;
}

void write_vdex_header(FileHandle& fh,
                       VdexVersion vdex_version,
                       uint32_t num_dex_files,
                       uint32_t dex_size,
                       uint32_t verifier_deps_size,
                       uint32_t quickening_info_size,
                       uint32_t vdex_checksum) {
  write_word(fh, kVdexMagicNum);
  write_word(fh, static_cast<uint32_t>(vdex_version));
  write_word(fh, num_dex_files);
  write_word(fh, dex_size);
  write_word(fh, verifier_deps_size);
  write_word(fh, quickening_info_size);
  write_word(fh, vdex_checksum);
}

template <typename DexFileListingType>
OatFile::Status build_vdex_odex_pairs(const std::string& oat_file_name,
                                      OatVersion oat_version,
                                      const DexInput& dex_input,
                                      InstructionSet isa,
                                      bool write_elf,
                                      const std::string& art_image_location,
                                      bool samsung_mode,
                                      const QuickData* quick_data) {
  const std::vector<KeyValueStore::KeyValue> key_value = {
      {"classpath", ""},
      {"compiler-filter", "assume-verified"},
      // Oreo will reject any OAT file that doesn't set this flag.
      {"concurrent-copying", "true"},
      {"debuggable", "false"},
      // What ever will happen if art tries to use this?
      {"dex2oat-cmdline", "--oat-file=/dev/null --dex-file=/dev/null"},
      {"dex2oat-host", "X86"},
      {"has-patch-info", "false"},
      {"native-debuggable", "false"},
      {"image-location", art_image_location.c_str()},
      {"pic", "false"},
      {kCreatedByOatmeal, "true"}};

  std::vector<DexInput> single_dex_input;
  single_dex_input.push_back(dex_input);

  ////////// Compute sizes and offsets.
  const auto keyvalue_size = KeyValueStore::compute_size(key_value);
  const auto dex_file_listing_size =
      DexFileListingType::compute_size(single_dex_input, samsung_mode);

  // Neither the keyvalue store or the DexFileListing require alignment.
  uint32_t oat_dex_files_offset = OatHeader::size(oat_version) + keyvalue_size;
  uint32_t next_offset = align<4>(OatHeader::size(oat_version) + keyvalue_size +
                                  dex_file_listing_size);

  auto dex_files =
      DexFileListingType::build(single_dex_input, next_offset, samsung_mode);

  auto oat_size = align<0x1000>(next_offset);
  auto header = build_header(
      oat_version, single_dex_input, isa, keyvalue_size, oat_size, nullptr);

  printf("Oat Size: %u\n", oat_size);

  ////////// Write the file.

  auto oat_fh = FileHandle(fopen(oat_file_name.c_str(), "w"));
  if (oat_fh.get() == nullptr) {
    return OatFile::Status::BUILD_IO_ERROR;
  }

  if (write_elf) {
    write_padding(oat_fh, 0, 0x1000);
    oat_fh.set_seek_reference_to_fpos();
    oat_fh.reset_bytes_written();
  }

  if (oat_version == OatVersion::V_131) {
    CHECK(oat_dex_files_offset != 0, "OatDexFiles offset can't be zero");
    header.oat_dex_files_offset = oat_dex_files_offset;
  }

  header.write(oat_fh);

  // Write key value store.
  KeyValueStore::write(oat_fh, key_value);

  // write DexFileListing
  DexFileListingType::write(oat_fh, dex_files, samsung_mode);

  // Write padding to align to 4 bytes.
  auto padding = align<4>(oat_fh.bytes_written()) - oat_fh.bytes_written();
  char buf[4] = {};
  write_buf(oat_fh, ConstBuffer{buf, padding});

  // Write lookup tables.
  CHECK(oat_file_name.substr(oat_file_name.size() - 5) == std::string(".odex"),
        "V124/V131 Oatmeal should generate .odex files");

  auto vdex_file_name =
      oat_file_name.substr(0, oat_file_name.size() - 4) + std::string("vdex");

  printf("VDEX output file: %s\n", vdex_file_name.c_str());

  const auto& dex_input_filename = dex_input.filename;

  // This will open the DEX file twice, we need its size first to
  // write it in the VDEX header.
  uint32_t dex_file_size = 0;

  auto dex_fh = FileHandle(fopen(dex_input_filename.c_str(), "r"));
  dex_file_size = get_filesize(dex_fh);

  // Retrieve the DEX checksum to store it just after the VDEX header
  uint32_t dex_checksum = 0x0;
  dex_fh.seek_set(8);
  dex_fh.fread(&dex_checksum, sizeof(uint32_t), 1);
  dex_fh.seek_set(0);

  auto vdex_fh = FileHandle(fopen(vdex_file_name.c_str(), "w"));

  write_vdex_header(
      vdex_fh, vdexVersion(oat_version), 1, dex_file_size, 0, 0, dex_checksum);
  write_dex_file(dex_input, quick_data, vdex_fh);

  OatClasses_124::write(dex_files, oat_fh);

  LookupTables::write(single_dex_input, dex_files, oat_fh);

  ////////// Update header with final checksum.
  CHECK(oat_fh.seek_begin());

  // Note: So far, I can't replicate the checksum computation done by
  // dex2oat. It appears that the file is written in a fairly arbitrary
  // order, and the checksum is computed as those sections are written.
  // Fortunately, art does not seem to verify the checksum at any point.
  // We don't even attempt to compute the checksum now, as it takes a few
  // seconds to do so.
  header.common.adler32_checksum = 0xcdcdcdcd;

  write_obj(oat_fh, header.common);

  if (write_elf) {
    oat_fh.set_seek_reference(0);
    oat_fh.seek_begin();

    ElfWriter section_headers(oat_version);
    section_headers.build(
        isa, oat_size, compute_bss_size_079(single_dex_input));
    section_headers.write(oat_fh);
  }

  return OatFile::Status::BUILD_SUCCESS;
}

template <typename DexFileListinType>
OatFile::Status build_oatfile_after_v124(const std::string& oat_file_name,
                                         const std::vector<DexInput>& dex_input,
                                         const OatVersion oat_version,
                                         InstructionSet isa,
                                         bool write_elf,
                                         const std::string& art_image_location,
                                         bool samsung_mode,
                                         const QuickData* quick_data) {
  // Make sure the output is a directory where we will place ODEX and VDEX files
  CHECK(oat_file_name[oat_file_name.size() - 1] == '/');
  OatFile::Status result = OatFile::Status::BUILD_SUCCESS;

  for (const auto& dex : dex_input) {
    size_t found = dex.filename.find_last_of('/') + 1;
    CHECK(found >= 0);
    auto odex_file_name = dex.filename.substr(found);
    odex_file_name.erase(odex_file_name.size() - 3);
    odex_file_name = oat_file_name + odex_file_name + std::string("odex");

    CHECK(oat_version == OatVersion::V_124 || oat_version == OatVersion::V_131,
          "must not build vdex/odex pairs for non-Oreo builds");

    auto partial_result =
        build_vdex_odex_pairs<DexFileListinType>(odex_file_name,
                                                 oat_version,
                                                 dex,
                                                 isa,
                                                 write_elf,
                                                 art_image_location,
                                                 samsung_mode,
                                                 quick_data);

    if (partial_result != OatFile::Status::BUILD_SUCCESS) {
      fprintf(stderr,
              "Building V124/V131 ODEX/VDEX pair failed for DEX input: %s, "
              "Result: %d\n",
              dex.filename.c_str(),
              static_cast<int>(partial_result));
      result = partial_result;
    }
  }
  return result;
}

OatFile::Status OatFile_064::build(const std::string& oat_file_name,
                                   const std::vector<DexInput>& dex_input,
                                   const OatVersion oat_version,
                                   InstructionSet isa,
                                   bool write_elf,
                                   const std::string& art_image_location,
                                   bool samsung_mode,
                                   const QuickData* quick_data) {
  return build_oatfile<DexFileListing_064,
                       OatClasses_064,
                       LookupTables_Nil,
                       SamsungLookupTables>(oat_file_name,
                                            dex_input,
                                            oat_version,
                                            isa,
                                            write_elf,
                                            art_image_location,
                                            samsung_mode,
                                            quick_data);
}

OatFile::Status OatFile_079::build(const std::string& oat_file_name,
                                   const std::vector<DexInput>& dex_input,
                                   const OatVersion oat_version,
                                   InstructionSet isa,
                                   bool write_elf,
                                   const std::string& art_image_location,
                                   bool samsung_mode,
                                   const QuickData* quick_data) {
  return build_oatfile<DexFileListing_079,
                       OatClasses_079,
                       LookupTables,
                       SamsungLookupTablesNil>(oat_file_name,
                                               dex_input,
                                               oat_version,
                                               isa,
                                               write_elf,
                                               art_image_location,
                                               samsung_mode,
                                               quick_data);
}

OatFile::Status OatFile_124::build(const std::string& oat_file_name,
                                   const std::vector<DexInput>& dex_input,
                                   const OatVersion oat_version,
                                   InstructionSet isa,
                                   bool write_elf,
                                   const std::string& art_image_location,
                                   bool samsung_mode,
                                   const QuickData* quick_data) {
  return build_oatfile_after_v124<DexFileListing_124>(oat_file_name,
                                                      dex_input,
                                                      oat_version,
                                                      isa,
                                                      write_elf,
                                                      art_image_location,
                                                      samsung_mode,
                                                      quick_data);
}

OatFile::Status OatFile_131::build(const std::string& oat_file_name,
                                   const std::vector<DexInput>& dex_input,
                                   const OatVersion oat_version,
                                   InstructionSet isa,
                                   bool write_elf,
                                   const std::string& art_image_location,
                                   bool samsung_mode,
                                   const QuickData* quick_data) {
  return build_oatfile_after_v124<DexFileListing_131>(oat_file_name,
                                                      dex_input,
                                                      oat_version,
                                                      isa,
                                                      write_elf,
                                                      art_image_location,
                                                      samsung_mode,
                                                      quick_data);
}

OatFile::Status OatFile::build(const std::vector<std::string>& oat_file_names,
                               const std::vector<DexInput>& dex_files,
                               const std::string& oat_version,
                               const std::string& arch,
                               bool write_elf,
                               const std::string& art_image_location,
                               bool samsung_mode,
                               const std::string& quick_data_location) {
  std::unique_ptr<QuickData> quick_metadata =
      read_quick_data(quick_data_location);
  auto build_fn = [&](const std::string& oat_file_name,
                      const std::vector<DexInput>& dexes) {
    auto version = versionInt(oat_version);
    auto isa = instruction_set(arch);
    switch (version) {
    case OatVersion::V_079:
    case OatVersion::V_088:
      return OatFile_079::build(oat_file_name,
                                dexes,
                                version,
                                isa,
                                write_elf,
                                art_image_location,
                                samsung_mode,
                                quick_metadata.get());

    case OatVersion::V_039:
    case OatVersion::V_045:
    case OatVersion::V_064:
    case OatVersion::V_067:
      return OatFile_064::build(oat_file_name,
                                dexes,
                                version,
                                isa,
                                write_elf,
                                art_image_location,
                                samsung_mode,
                                quick_metadata.get());
    case OatVersion::V_124:
      return OatFile_124::build(oat_file_name,
                                dexes,
                                version,
                                isa,
                                write_elf,
                                art_image_location,
                                samsung_mode,
                                quick_metadata.get());
    case OatVersion::V_131:
      return OatFile_131::build(oat_file_name,
                                dexes,
                                version,
                                isa,
                                write_elf,
                                art_image_location,
                                samsung_mode,
                                quick_metadata.get());

    default:
      fprintf(stderr, "version 0x%08x unknown\n", static_cast<int>(version));
      return Status::BUILD_UNSUPPORTED_VERSION;
    }
  };

  if (oat_file_names.empty()) {
    fprintf(stderr, "At least one oat file name required\n");
    return Status::BUILD_ARG_ERROR;
  } else if (oat_file_names.size() == 1) {
    return build_fn(oat_file_names[0], dex_files);
  } else {
    if (oat_file_names.size() != dex_files.size()) {
      fprintf(stderr, "One oat file per dex file required.\n");
      return Status::BUILD_ARG_ERROR;
    }

    for (unsigned int i = 0; i < oat_file_names.size(); i++) {
      std::vector<DexInput> dex_file;
      dex_file.push_back(dex_files[i]);
      auto status = build_fn(oat_file_names[i], dex_file);
      if (status != Status::BUILD_SUCCESS) {
        return status;
      }
    }
    return Status::BUILD_SUCCESS;
  }
}
