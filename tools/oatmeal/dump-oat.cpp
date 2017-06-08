/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "dump-oat.h"
#include "elf-writer.h"
#include "memory-accounter.h"
#include "util.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#define PACKED __attribute__((packed))

#define READ_WORD(dest, ptr)               \
  do {                                     \
    cur_ma()->memcpyAndMark((dest), ptr, 4); \
    ptr += 4;                              \
  } while (false);

namespace {

constexpr uint32_t kOatMagicNum = 0x0a74616F;

constexpr uint32_t kOatVersionUnknown = 0;
constexpr uint32_t kOatVersion064 = 0x00343630;
constexpr uint32_t kOatVersion079 = 0x00393730;
constexpr uint32_t kOatVersion088 = 0x00383830;

uint32_t versionInt(const std::string& version_str) {
  if (version_str == "064") {
    return kOatVersion064;
  } else if (version_str == "079") {
    return kOatVersion079;
  } else if (version_str == "088") {
    return kOatVersion088;
  } else {
    CHECK(false, "Bad version %s", version_str.c_str());
  }
  return kOatVersionUnknown;
}

struct PACKED OatHeader_Common {
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
    char magic_str[5] = {};
    char version_str[5] = {};
    memcpy(magic_str, &magic, 3); // magic has a newline character at idx 4.
    memcpy(version_str, &version, 4);

    printf("  magic:   0x%08x '%s'\n", magic, magic_str);
    printf("  version: 0x%08x '%s'\n", version, version_str);
    printf("  checksum: 0x%08x\n", adler32_checksum);
  }
};

// Oat header for versions 064, 079, 088.
struct PACKED OatHeader_064 {
  OatHeader_Common common;

  InstructionSet instruction_set = InstructionSet::kNone;
  uint32_t instruction_set_features_bitmap = 0;
  uint32_t dex_file_count = 0;
  uint32_t executable_offset = 0;
  uint32_t interpreter_to_interpreter_bridge_offset = 0;
  uint32_t interpreter_to_compiled_code_bridge_offset = 0;
  uint32_t jni_dlsym_lookup_offset = 0;
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

  static OatHeader_064 parse(ConstBuffer buf) {
    OatHeader_064 header;
    CHECK(buf.len >= sizeof(OatHeader_064));
    cur_ma()->memcpyAndMark(&header, buf.ptr, sizeof(OatHeader_064));

    CHECK(header.common.magic == kOatMagicNum);
    CHECK(header.common.version == kOatVersion064 ||
          header.common.version == kOatVersion079 ||
          header.common.version == kOatVersion088);

    return header;
  }

  void update_checksum(Adler32& cksum) {
    auto begin = reinterpret_cast<const char*>(&instruction_set);
    auto end = reinterpret_cast<const char*>(&key_value_store[0]);
    cksum.update(begin, end - begin);
  }

  void write(FileHandle& fh) {
    write_buf(fh, ConstBuffer { reinterpret_cast<const char*>(this),
          sizeof(OatHeader_064) });
  }

  void print() {
    common.print();
    printf(
        "  isa: %s\n"
        "  isa features bitmap: 0x%08x\n"
        "  dex_file_count: 0x%08x\n"
        "  executable_offset: 0x%08x\n"
        "  interpreter_to_interpreter_bridge_offset: 0x%08x\n"
        "  interpreter_to_compiled_code_bridge_offset: 0x%08x\n"
        "  jni_dlsym_lookup_offset: 0x%08x\n"
        "  quick_generic_jni_trampoline_offset: 0x%08x\n"
        "  quick_imt_conflict_trampoline_offset: 0x%08x\n"
        "  quick_resolution_trampoline_offset: 0x%08x\n"
        "  quick_to_interpreter_bridge_offset: 0x%08x\n"
        "  image_patch_delta: 0x%08x\n"
        "  image_file_location_oat_checksum: 0x%08x\n"
        "  image_file_location_oat_data_begin: 0x%08x\n"
        "  key_value_store_size: 0x%08x\n",
        instruction_set_str(instruction_set),
        instruction_set_features_bitmap,
        dex_file_count,
        executable_offset,
        interpreter_to_interpreter_bridge_offset,
        interpreter_to_compiled_code_bridge_offset,
        jni_dlsym_lookup_offset,
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
        kv.first = std::string(&buf[next], len);
        next += len;
        remaining -= len;
      }
      if (remaining <= 0) {
        break;
      }
      // value
      {
        auto len = strnlen(&buf[next], remaining) + 1;
        kv.second = std::string(&buf[next], len);
        next += len;
        remaining -= len;
      }
      kv_pairs_.push_back(std::move(kv));
    }
  }

  void print() {
    for (const auto& e : kv_pairs_) {
      printf("  %s: %s\n", e.first.c_str(), e.second.c_str());
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

 private:
  std::vector<KeyValue> kv_pairs_;
};

struct PACKED DexClassDef {
  uint16_t class_idx;
  uint16_t pad1;
  uint32_t access_flags;
  uint16_t superclass_idx;
  uint16_t pad2;
  uint32_t interfaces_off;
  uint32_t source_file_idx;
  uint32_t annotations_off;
  uint32_t class_data_off;
  uint32_t static_values_off;
};

// Header for dex files. Note that this currently consumes the entire
// contents of the dex file (in addition to the header proper) for the
// purposes of memory-accounting.
struct PACKED DexFileHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t checksum;
  uint8_t signature[20];
  uint32_t file_size;
  uint32_t header_size;
  uint32_t endian_tag;
  uint32_t link_size;

  uint32_t link_off;
  uint32_t map_off;
  uint32_t string_ids_size;
  uint32_t string_ids_off;
  uint32_t type_ids_size;
  uint32_t type_ids_off;
  uint32_t proto_ids_size;
  uint32_t proto_ids_off;
  uint32_t field_ids_size;
  uint32_t field_ids_off;
  uint32_t method_ids_size;
  uint32_t method_ids_off;
  uint32_t class_defs_size;
  uint32_t class_defs_off;
  uint32_t data_size;
  uint32_t data_off;

  static DexFileHeader parse(ConstBuffer buf) {
    DexFileHeader header;
    memcpy(&header, buf.ptr, sizeof(DexFileHeader));

    // Mark the whole file consumed.
    cur_ma()->markRangeConsumed(buf.ptr, header.file_size);

    return header;
  }
};

// DexIdBufs handles looking up class names in dex files within in-memory oat files.
class DexIdBufs {
public:
  // Note: DexIdBufs must not outlive the memory wrapped by oat_buf.
  DexIdBufs(ConstBuffer oat_buf, uint32_t dex_offset, const DexFileHeader& header) {
    dex_buf_ = oat_buf.slice(dex_offset);
    auto class_defs_buf = dex_buf_.slice(header.class_defs_off);
    auto type_ids_buf = dex_buf_.slice(header.type_ids_off);
    auto string_ids_buf = dex_buf_.slice(header.string_ids_off);

    // We memcpy into new buffers since the data in the dex may not be aligned.
    class_defs_.reset(new DexClassDef[header.class_defs_size]);
    type_ids_.reset(new uint32_t[header.type_ids_size]);
    string_ids_.reset(new uint32_t[header.string_ids_size]);

    memcpy(class_defs_.get(), class_defs_buf.ptr, header.class_defs_size * sizeof(DexClassDef));
    memcpy(type_ids_.get(), type_ids_buf.ptr, header.type_ids_size * sizeof(uint32_t));
    memcpy(string_ids_.get(), string_ids_buf.ptr, header.string_ids_size * sizeof(uint32_t));
  }

  std::string get_class_name(int i) {
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
};

// Class meta data for all the classes that appear in the dex files.
// - DexFileListing specifies the beginning of the class listing for
//   each dex file.
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
    case Status::kStatusRetired: return "O";
    case Status::kStatusError: return "E";
    case Status::kStatusNotReady: return "N";
    case Status::kStatusIdx: return "I";
    case Status::kStatusLoaded: return "L";
    case Status::kStatusResolving: return "r";
    case Status::kStatusResolved: return "R";
    case Status::kStatusVerifying: return "v";
    case Status::kStatusRetryVerificationAtRuntime: return "v";
    case Status::kStatusVerifyingAtRuntime: return "v";
    case Status::kStatusVerified: return "V";
    case Status::kStatusInitializing: return "i";
    case Status::kStatusInitialized: return "I";
    case Status::kStatusMax: return "M";
    default: return "?";
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
  struct PACKED ClassInfo {
    ClassInfo() = default;
    ClassInfo(Status s, Type t) :
      status(static_cast<int16_t>(s)), type(static_cast<uint16_t>(t)) {}
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
    DexFile(std::string location_, uint32_t location_checksum_,
            uint32_t file_offset_)
      : location(location_), location_checksum(location_checksum_),
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
    DexFile_079(std::string location_, uint32_t location_checksum_,
                uint32_t file_offset_, uint32_t num_classes_,
                uint32_t classes_offset_, uint32_t lookup_table_offset_)
    : DexFile(location_, location_checksum_, file_offset_),
      num_classes(num_classes_),
      classes_offset(classes_offset_), lookup_table_offset(lookup_table_offset_) {
    }

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
    for (const auto& e : dex_files_) {
      printf("  {\n");
      printf("    location: %s\n", e.location.c_str());
      printf("    location_checksum: 0x%08x\n", e.location_checksum);
      printf("    file_offset: 0x%08x\n", e.file_offset);
      printf("    classes_offset: 0x%08x\n", e.classes_offset);
      printf("    lookup_table_offset: 0x%08x\n", e.lookup_table_offset);
      printf("  }\n");
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

  static uint32_t compute_size(const std::vector<DexInput>& dex_input) {

    // Locations are *not* null terminated.
    const auto num_files = dex_input.size();
    uint32_t total_file_location_size = 0;
    for (const auto& e : dex_input) {
      total_file_location_size += e.location.size();
    }
    return total_file_location_size
         + num_files * sizeof(uint32_t)  // location len
         + num_files * sizeof(uint32_t)  // location checksum
         + num_files * sizeof(uint32_t)  // file offset
         + num_files * sizeof(uint32_t)  // classes offset
         + num_files * sizeof(uint32_t); // lookup_table_offset
  }

  static std::vector<DexFile_079> build(
      const std::vector<DexInput>& dexes, uint32_t& next_offset);

  static void write(FileHandle& fh,
                    const std::vector<DexFileListing_079::DexFile_079>& dex_files) {
    for (const auto& file : dex_files) {
      uint32_t location_len = file.location.size();
      write_word(fh, location_len);

      ConstBuffer location { file.location.c_str(), location_len };
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

// Dex File listing for OAT versions 064
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
//    classes:             Variable length table of class status information.
//                         Length depends on the number of classes in the dex file.
//    classes_offset:      4 unsigned bytes, offset from beginning of OAT file
//                         where class metadata listing (OatClasses) for this
//                         dex file begins.
//    lookup_table_offset: 4 unsigned bytes, offset from beginning of OAT file
//                         where the class lookup table (LookupTables) for this
//                         dex file begins.
class DexFileListing_064 : public DexFileListing {
 public:
  MOVABLE(DexFileListing_064);

  using ClassInfo = OatClasses::ClassInfo;

  struct DexFile_064 : public DexFile {
    std::vector<ClassInfo> class_info;
    std::vector<std::string> class_names;
  };

  DexFileListing_064(int numDexFiles, ConstBuffer buf, ConstBuffer oat_buf) {
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

      auto dex_header = DexFileHeader::parse(oat_buf.slice(file.file_offset));
      const auto num_classes = dex_header.class_defs_size;
      file.class_info.reserve(num_classes);

      DexIdBufs id_bufs(oat_buf, file.file_offset, dex_header);

      for (unsigned int i = 0; i < num_classes; i++) {
        uint32_t class_info_offset;
        READ_WORD(&class_info_offset, ptr);

        ClassInfo class_info;
        cur_ma()->memcpyAndMark(&class_info, oat_buf.ptr + class_info_offset,
                                sizeof(ClassInfo));
        file.class_info.push_back(class_info);
        file.class_names.push_back(id_bufs.get_class_name(i));
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
        printf("%s%s ",
               OatClasses::shortStatusStr(static_cast<OatClasses::Status>(info.status)),
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
      foreach_pair(e.class_info, e.class_names,
        [&](const ClassInfo& info, const std::string& name) {
          if (info.status < static_cast<int>(OatClasses::Status::kStatusVerified)) {
            printf("    %s unverified (status: %s)\n",
                   name.c_str(), OatClasses::statusStr(info.status));
          }
        }
      );
    }
  }

  std::vector<uint32_t> dex_file_offsets() const override {
    std::vector<uint32_t> ret;
    for (const auto& f : dex_files_) {
      ret.push_back(f.file_offset);
    }
    return ret;
  }

 private:
  std::vector<DexFile_064> dex_files_;
};

// Collection of all the headers of all the dex files found in the oat.
class DexFiles {
 public:
  MOVABLE(DexFiles);

  // buf should start at the beginning of the OAT file, as the offsets
  // in DexFileListing are relative to the beginning of the OAT file.
  DexFiles(const DexFileListing& dex_file_listing, ConstBuffer buf) {
    for (const auto& file_offset : dex_file_listing.dex_file_offsets()) {
      auto dex_file_buf = buf.slice(file_offset);
      headers_.push_back(DexFileHeader::parse(dex_file_buf));
    }
  }

  void print() {
    for (const auto& e : headers_) {
      printf("  { DexFile\n");
      printf("    file_size: 0x%08x\n", e.file_size);
      printf("    num_classes: 0x%08x\n", e.class_defs_size);
      printf("  }\n");
    }
  }

  const std::vector<DexFileHeader>& headers() const { return headers_; }

 private:
  std::vector<DexFileHeader> headers_;
};

// Note: There is no separate 0atClasses_064, since in 064 this data is
// part of DexFileListing_064.
class OatClasses_079 : public OatClasses {
public:
  struct DexClasses {
    std::string dex_file;
    std::vector<ClassInfo> class_info;
    std::vector<std::string> class_names;
  };

  OatClasses_079(const DexFileListing_079& dex_file_listing,
                 const DexFiles& dex_files,
                 ConstBuffer oat_buf);

  MOVABLE(OatClasses_079);

  void print();

  void print_unverified_classes();

  static void write(const std::vector<DexFileListing_079::DexFile_079>& dex_files,
                    ChecksummingFileHandle& cksum_fh);

private:
  std::vector<DexClasses> classes_;
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
          CHECK(info.type ==
                    static_cast<uint16_t>(Type::kOatClassNoneCompiled),
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
    foreach_pair(e.class_info, e.class_names,
      [&](const ClassInfo& info, const std::string& name) {
        if (info.status < static_cast<int>(Status::kStatusVerified)) {
          printf("    %s unverified (status: %s)\n",
                 name.c_str(), statusStr(info.status));
        }
      }
    );
  }
}

void OatClasses_079::write(
    const std::vector<DexFileListing_079::DexFile_079>& dex_files,
    ChecksummingFileHandle& cksum_fh) {
  for (const auto& dex_file : dex_files) {
    CHECK(dex_file.classes_offset == cksum_fh.bytes_written());
    const auto num_classes = dex_file.num_classes;
    uint32_t table_offset = dex_file.classes_offset
                      + num_classes * sizeof(uint32_t);

    // write pointers to ClassInfo.
    for (unsigned int i = 0; i < num_classes; i++) {
      write_word(cksum_fh, table_offset + i * sizeof(uint32_t));
    }
    CHECK(table_offset == cksum_fh.bytes_written());

    // Write ClassInfo structs.
    OatClasses::ClassInfo info(OatClasses::Status::kStatusVerified,
                               OatClasses::Type::kOatClassNoneCompiled);
    for (unsigned int i = 0; i < num_classes; i++) {
      write_obj(cksum_fh, info);
      table_offset += sizeof(OatClasses::ClassInfo);
    }
    CHECK(table_offset == cksum_fh.bytes_written());
  }
}

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
    uint32_t str_offset; // the offset, relative to the beginning of the dexfile,
                         // where the name of the class begins.
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

      tables_.push_back(
          LookupTable{listing_it->file_offset,
                      listing_it->location,
                      reinterpret_cast<const LookupTableEntry*>(ptr),
                      num_entries});

      ++listing_it;
      ++file_it;
    }
  }

  void print() {
    for (const auto& e : tables_) {
      printf("  { Type lookup table %s\n", e.dex_file.c_str());
      printf("    num_entries: %u\n", e.num_entries);
      for (unsigned int i = 0; i < e.num_entries; i++) {
        const auto& entry = e.entries[i];
        if (entry.str_offset != 0) {
          printf("    {\n");
          printf("    str: %s\n",
                 oat_buf_.slice(e.dex_file_offset + entry.str_offset).ptr);
          printf("    str offset: 0x%08x\n", entry.str_offset);
          printf("    }\n");
        }
      }
      printf("  }\n");
    }
  }

  static uint32_t numEntries(uint32_t num_classes) {
    return supportedSize(num_classes) ? nextPowerOfTwo(num_classes) : 0u;
  }

  static void write(const std::vector<DexInput>& dex_input,
                    const std::vector<DexFileListing_079::DexFile_079>& dex_files,
                    ChecksummingFileHandle& cksum_fh) {
    foreach_pair(dex_input, dex_files,
      [&](const DexInput& dex_input, const DexFileListing_079::DexFile_079& dex_file) {
        CHECK(dex_file.lookup_table_offset == cksum_fh.bytes_written());
        const auto num_classes = dex_file.num_classes;

        const auto lookup_table_size = numEntries(num_classes);
        const auto lookup_table_byte_size = lookup_table_size * sizeof(LookupTableEntry);

        auto lookup_table_buf = build_lookup_table(dex_input.filename, lookup_table_size);
        auto buf = ConstBuffer { reinterpret_cast<const char*>(lookup_table_buf.get()),
                                 lookup_table_byte_size };
        write_buf(cksum_fh, buf);
      }
    );
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

  static uint16_t make_lt_data(uint16_t class_def_idx, uint32_t hash, uint32_t mask) {
    uint16_t hash_mask = static_cast<uint16_t>(~mask);
    return (static_cast<uint16_t>(hash) & hash_mask) | class_def_idx;
  }

  static bool insert_no_probe(
      LookupTableEntry* table,
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

  static void insert(
      LookupTableEntry* table,
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

  static std::unique_ptr<LookupTableEntry[]>
  build_lookup_table(const std::string& filename, uint32_t lookup_table_size) {

    std::unique_ptr<LookupTableEntry[]>
      table_buf(new LookupTableEntry[lookup_table_size]);
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
    CHECK(dex_fh.fread(typeid_buf.get(), sizeof(uint32_t), num_type_ids)
        == num_type_ids);

    // Read the string ids array.
    const auto num_string_ids = header.string_ids_size;
    std::unique_ptr<uint32_t[]> stringid_buf(new uint32_t[num_string_ids]);
    CHECK(dex_fh.seek_set(header.string_ids_off));
    CHECK(dex_fh.fread(stringid_buf.get(), sizeof(uint32_t), num_string_ids)
        == num_string_ids);

    CHECK(dex_fh.seek_set(header.class_defs_off));

    std::unique_ptr<DexClassDef[]> class_defs_buf(new DexClassDef[num_classes]);
    CHECK(dex_fh.fread(class_defs_buf.get(), sizeof(DexClassDef), num_classes)
        == num_classes);

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
      auto read_size = dex_fh.fread(class_name_buf, sizeof(char), kClassNameBufSize);
      CHECK(read_size > 0);

      auto ptr = class_name_buf;
      const auto str_size = read_uleb128(&ptr) + 1;
      const auto str_start = ptr - class_name_buf;

      std::string class_name;

      if (str_start + str_size >= kClassNameBufSize) {
        std::unique_ptr<char[]> large_class_name_buf(new char[str_size]);
        dex_fh.seek_set(string_offset + str_start);
        CHECK(dex_fh.fread(large_class_name_buf.get(), sizeof(char), str_size)
            == str_size);
        class_name = std::string(large_class_name_buf.get(), str_size);
      } else {
        class_name = std::string(ptr, str_size);
      }

      const auto hash = hash_str(class_name);
      const auto data = make_lt_data(i, hash, mask);

      if (!insert_no_probe(table_buf.get(),
            LookupTableEntry { string_offset, data, 0 }, hash, mask)) {
        retry_indices.emplace_back(Retry { string_offset, data, hash });
      }
    }

    for (const auto& retry : retry_indices) {
      insert(table_buf.get(),
             LookupTableEntry { retry.string_offset, retry.data, 0 },
             retry.hash, mask);
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

class OatFile_064 : public OatFile {
 public:
  UNCOPYABLE(OatFile_064);
  MOVABLE(OatFile_064);

  static std::unique_ptr<OatFile> parse(ConstBuffer buf) {
    auto header = OatHeader_064::parse(buf);
    auto key_value_store = KeyValueStore(
        buf.slice(sizeof(header)).truncate(header.key_value_store_size));

    auto rest = buf.slice(sizeof(header) + header.key_value_store_size);
    DexFileListing_064 dfl(header.dex_file_count, rest, buf);

    DexFiles dex_files(dfl, buf);

    return std::unique_ptr<OatFile>(new OatFile_064(header,
                                                    key_value_store,
                                                    std::move(dfl),
                                                    std::move(dex_files)));
  }

  void print(bool dump_classes, bool dump_tables, bool print_unverified_classes) override {
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

 private:
  OatFile_064(OatHeader_064 h,
              KeyValueStore kv,
              DexFileListing_064 dfl,
              DexFiles dex_files)
      : header_(h),
        key_value_store_(kv),
        dex_file_listing_(std::move(dfl)),
        dex_files_(std::move(dex_files)) {}

  OatHeader_064 header_;
  KeyValueStore key_value_store_;
  DexFileListing_064 dex_file_listing_;
  DexFiles dex_files_;
};

// OatFile format for 079 and 088. (088 may have changes that don't
// show up with verify-none. So far it appears to be identical.)
class OatFile_079 : public OatFile {
 public:
  UNCOPYABLE(OatFile_079);
  MOVABLE(OatFile_079);

  static std::unique_ptr<OatFile> parse(ConstBuffer buf) {

    auto header = OatHeader_064::parse(buf);
    auto key_value_store = KeyValueStore(
        buf.slice(sizeof(header)).truncate(header.key_value_store_size));

    auto rest = buf.slice(sizeof(header) + header.key_value_store_size);
    DexFileListing_079 dfl(header.dex_file_count, rest);

    DexFiles dex_files(dfl, buf);

    LookupTables lookup_tables(dfl, dex_files, buf);

    OatClasses_079 oat_classes(dfl, dex_files, buf);

    return std::unique_ptr<OatFile>(new OatFile_079(header,
                                                    key_value_store,
                                                    std::move(dfl),
                                                    std::move(dex_files),
                                                    std::move(lookup_tables),
                                                    std::move(oat_classes)));
  }

  void print(bool dump_classes, bool dump_tables, bool print_unverified_classes) override {
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
                      const uint32_t oat_version,
                      InstructionSet isa,
                      bool write_elf);

 private:
  OatFile_079(OatHeader_064 h,
              KeyValueStore kv,
              DexFileListing_079 dfl,
              DexFiles dex_files,
              LookupTables lt,
              OatClasses_079 oat_classes)
      : header_(h),
        key_value_store_(kv),
        dex_file_listing_(std::move(dfl)),
        dex_files_(std::move(dex_files)),
        lookup_tables_(std::move(lt)),
        oat_classes_(std::move(oat_classes)) {}

  OatHeader_064 header_;
  KeyValueStore key_value_store_;
  DexFileListing_079 dex_file_listing_;
  DexFiles dex_files_;
  LookupTables lookup_tables_;
  OatClasses_079 oat_classes_;
};

class OatFile_Unknown : public OatFile {
public:
  void print(bool dump_classes, bool dump_tables, bool print_unverified_classes) override {
    printf("Unknown OAT file version!\n");
    header_.print();
  }

  Status status() override { return Status::PARSE_UNKNOWN_VERSION; }

  static std::unique_ptr<OatFile> parse(ConstBuffer buf) {
    return std::unique_ptr<OatFile>(new OatFile_Unknown(buf));
  }

private:
  explicit OatFile_Unknown(ConstBuffer buf) {
    header_ = OatHeader_Common::parse(buf);
  }

  OatHeader_Common header_;
};

class OatFile_Bad : public OatFile {
public:
  void print(bool dump_classes, bool dump_tables, bool print_unverified_classes) override {
    printf("Bad magic number:\n");
    header_.print();
  }

  Status status() override { return Status::PARSE_BAD_MAGIC_NUMBER; }

  static std::unique_ptr<OatFile> parse(ConstBuffer buf) {
    return std::unique_ptr<OatFile>(new OatFile_Bad(buf));
  }

private:
  explicit OatFile_Bad(ConstBuffer buf) {
    header_ = OatHeader_Common::parse(buf);
  }

  OatHeader_Common header_;
};

}

OatFile::~OatFile() = default;

std::unique_ptr<OatFile> OatFile::parse(ConstBuffer buf) {

  if (buf.len >= sizeof(Elf32_Ehdr)) {
    Elf32_Ehdr header;
    memcpy(&header, buf.ptr, sizeof(Elf32_Ehdr));
    if (header.e_ident[0] == 0x7f &&
        header.e_ident[1] == 'E' &&
        header.e_ident[2] == 'L' &&
        header.e_ident[3] == 'F') {
      // .rodata starts at 0x1000 in every version of ART that i've seen.
      // If there are any where this isn't true, we'll have to actually read
      // out the offset of .rodata.
      buf = buf.slice(0x1000);
    }
  }

  auto header = OatHeader_Common::parse(buf);

  // TODO: do we need to handle endian-ness? I think all platforms we
  // care about are little-endian.
  if (header.magic != kOatMagicNum) {
    return OatFile_Bad::parse(buf);
  }

  switch (header.version) {
  case kOatVersion064:
    return OatFile_064::parse(buf);
  case kOatVersion079:
  case kOatVersion088:
    // 079 and 088 are the same as far as I can tell.
    return OatFile_079::parse(buf);
  default:
    return OatFile_Unknown::parse(buf);
  }
}

////////// building

OatHeader_064 build_header(const std::vector<DexInput>& dex_input,
                           InstructionSet isa,
                           uint32_t keyvalue_size,
                           uint32_t oat_size) {
  OatHeader_064 header;

  // the common portion of the header must be re-written after we've written
  // the rest of the file, as we don't know the checksum until then.
  // After we write the whole file, we come back and re-write the common header
  // to include the correct checksum.
  header.common.magic = 0xcdcdcdcd;
  header.common.version = 0xcdcdcdcd;
  header.common.adler32_checksum = 0xcdcdcdcd;

  header.instruction_set = isa;

  // This appears to be set to 1 on both x86 and arm, not clear if there is
  // ever a case where we need to parameterize this.
  header.instruction_set_features_bitmap = 1;

  header.dex_file_count = dex_input.size();

  header.executable_offset = oat_size;

  header.key_value_store_size = keyvalue_size;

  // omitted fields default to zero, and should remain zero.
  return header;
}

std::vector<DexFileListing_079::DexFile_079>
DexFileListing_079::build(const std::vector<DexInput>& dex_input,
                          uint32_t& next_offset) {
  uint32_t total_dex_size = 0;

  std::vector<DexFileListing_079::DexFile_079> dex_files;
  dex_files.reserve(dex_input.size());

  for (const auto dex : dex_input) {
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

    auto lookup_table_size =
      LookupTables::numEntries(num_classes) * sizeof(LookupTables::LookupTableEntry);

    dex_files.push_back(DexFileListing_079::DexFile_079(
      dex.location,
      header.checksum,
      dex_offset,
      // temporarily store sizes instead of offsets here.
      // they will be replaced with offsets in the next loop.
      num_classes,
      class_table_size,
      lookup_table_size
    ));
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

void write_dex_files(const std::vector<DexInput>& dex_input,
                     const std::vector<DexFileListing_079::DexFile_079>& dex_files,
                     ChecksummingFileHandle& cksum_fh) {
  foreach_pair(dex_input, dex_files,
    [&](const DexInput& input, const DexFileListing_079::DexFile_079& dex_file) {
      CHECK(dex_file.file_offset == cksum_fh.bytes_written());
      auto dex_fh = FileHandle(fopen(input.filename.c_str(), "r"));
      stream_file(dex_fh, cksum_fh);
    }
  );
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
    auto strings_offset = align<pointer_size>(meth_offset + methods_size(header.method_ids_size));
    auto fields_offset = align<pointer_size>(strings_offset + strings_size(header.string_ids_size));
    auto size = align<pointer_size>(fields_offset + fields_size(header.field_ids_size));
    ret += size;
  }
  return ret;
}


OatFile::Status OatFile_079::build(const std::string& oat_file_name,
                                   const std::vector<DexInput>& dex_input,
                                   const uint32_t oat_version,
                                   InstructionSet isa,
                                   bool write_elf) {

  const std::vector<KeyValueStore::KeyValue> key_value = {
    { "classpath", "" },
    { "compiler-filter", "verify-none" },
    { "debuggable", "false" },
    // What ever will happen if art tries to use this?
    { "dex2oat-cmdline", "--oat-file=/dev/null --dex-file=/dev/null" },
    { "dex2oat-host", "X86" },
    { "has-patch-info", "false" },
    { "native-debuggable", "false" },
    { "pic", "false" }
  };

  ////////// Compute sizes and offsets.

  const auto keyvalue_size = KeyValueStore::compute_size(key_value);
  const auto dex_file_listing_size = DexFileListing_079::compute_size(dex_input);

  // Neither the keyvalue store or the DexFileListing require alignment.
  uint32_t next_offset = align<4>(sizeof(OatHeader_064)
                                 + keyvalue_size
                                 + dex_file_listing_size);

  auto dex_files = DexFileListing_079::build(dex_input, next_offset);
  auto oat_size = align<0x1000>(next_offset);

  Adler32 cksum;
  auto header = build_header(dex_input, isa, keyvalue_size, oat_size);
  header.update_checksum(cksum);

  ////////// Write the file.

  auto oat_fh = FileHandle(fopen(oat_file_name.c_str(), "w"));
  if (oat_fh.get() == nullptr) {
    return Status::BUILD_IO_ERROR;
  }

  if (write_elf) {
    write_padding(oat_fh, 0, 0x1000);
    oat_fh.set_seek_reference_to_fpos();
    oat_fh.reset_bytes_written();
  }

  // Write header. Can't use ChecksummingFileHandle because the OatHeader_Common
  // portion of the header is not part of the checksum.
  header.write(oat_fh);

  // Destroy oat_fh, create cksum_fh
  ChecksummingFileHandle cksum_fh(std::move(oat_fh), std::move(cksum));

  // Write key value store.
  KeyValueStore::write(cksum_fh, key_value);

  // write DexFileListing
  DexFileListing_079::write(cksum_fh, dex_files);

  // Write padding to align to 4 bytes.
  auto padding = align<4>(cksum_fh.bytes_written()) - cksum_fh.bytes_written();
  char buf[4] = {};
  write_buf(cksum_fh, ConstBuffer { buf, padding });

  write_dex_files(dex_input, dex_files, cksum_fh);
  OatClasses_079::write(dex_files, cksum_fh);
  LookupTables::write(dex_input, dex_files, cksum_fh);

  // Pad with 0s up to oat_size
  // TODO: is the padding part of the checksum?
  write_padding(cksum_fh, 0, oat_size - cksum_fh.bytes_written());

  ////////// Update header with final checksum.

  const auto final_checksum = cksum_fh.cksum().get();

  // Note: the checksum in cksum_fh is garbage after this call.
  CHECK(cksum_fh.seek_begin());

  OatHeader_Common common_header;
  common_header.magic = kOatMagicNum;
  common_header.version = oat_version;
  // Note: So far, I can't replicate the checksum computation done by
  // dex2oat. It appears that the file is written in a fairly arbitrary
  // order, and the checksum is computed as those sections are written.
  // Fortunately, art does not seem to verify the checksum at any point.
  common_header.adler32_checksum = final_checksum;

  write_obj(cksum_fh, common_header);

  if (write_elf) {
    cksum_fh.set_seek_reference(0);
    cksum_fh.seek_begin();

    ElfWriter section_headers;
    section_headers.build(isa, oat_size, compute_bss_size_079(dex_input));
    section_headers.write(cksum_fh);
  }

  return Status::BUILD_SUCCESS;
}

OatFile::Status OatFile::build(const std::string& oat_file_name,
                               const std::vector<DexInput>& dex_files,
                               const std::string& oat_version,
                               const std::string& arch,
                               bool write_elf) {
  auto version = versionInt(oat_version);
  auto isa = instruction_set(arch);
  switch (version) {
    case kOatVersion079:
    case kOatVersion088:
        return OatFile_079::build(oat_file_name, dex_files, version, isa, write_elf);

    case kOatVersion064:
      fprintf(stderr, "version 064 not supported\n");
      return Status::BUILD_UNSUPPORTED_VERSION;

    default:
      fprintf(stderr, "version 0x%08x unknown\n", version);
      return Status::BUILD_UNSUPPORTED_VERSION;

  }
}
