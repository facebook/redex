/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "dump-oat.h"
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
constexpr uint32_t kOatVersion064 = 0x00343630;
constexpr uint32_t kOatVersion079 = 0x00393730;
constexpr uint32_t kOatVersion088 = 0x00383830;

enum class InstructionSet {
  kNone = 0,
  kArm = 1,
  kArm64 = 2,
  kThumb2 = 3,
  kX86 = 4,
  kX86_64 = 5,
  kMips = 6,
  kMips64 = 7
};

const char* instruction_set_str(InstructionSet isa) {
  switch (isa) {
  case InstructionSet::kNone:
    return "NONE";
  case InstructionSet::kArm:
    return "arm";
  case InstructionSet::kArm64:
    return "arm64";
  case InstructionSet::kThumb2:
    return "thumb2";
  case InstructionSet::kX86:
    return "x86";
  case InstructionSet::kX86_64:
    return "x86_64";
  case InstructionSet::kMips:
    return "mips";
  case InstructionSet::kMips64:
    return "mips64";
  default:
    return "<UKNOWN>";
  }
}

struct PACKED OatHeader_Common {
  uint32_t magic = 0;
  uint32_t version = 0;

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
  }
};

// Oat header for versions 064, 079, 088.
struct PACKED OatHeader_064 {
  OatHeader_Common common;

  uint32_t adler32_checksum;

  InstructionSet instruction_set;
  uint32_t instruction_set_features_bitmap;
  uint32_t dex_file_count;
  uint32_t executable_offset;
  uint32_t interpreter_to_interpreter_bridge_offset;
  uint32_t interpreter_to_compiled_code_bridge_offset;
  uint32_t jni_dlsym_lookup_offset;
  uint32_t quick_generic_jni_trampoline_offset;
  uint32_t quick_imt_conflict_trampoline_offset;
  uint32_t quick_resolution_trampoline_offset;
  uint32_t quick_to_interpreter_bridge_offset;

  int32_t image_patch_delta;

  uint32_t image_file_location_oat_checksum;
  uint32_t image_file_location_oat_data_begin;

  uint32_t key_value_store_size;

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

  void print() {
    common.print();
    printf(
        "  checksum: 0x%08x\n"
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
        adler32_checksum,
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
  explicit KeyValueStore(ConstBuffer buf) : buf_(buf) {
    cur_ma()->markBufferConsumed(buf_);
  }

  void print() {
    int remaining = buf_.len;
    size_t next = 0;
    while (remaining > 0) {
      // key
      {
        auto len = strnlen(&buf_[next], remaining) + 1;
        printf("  %s: ", &buf_[next]);
        next += len;
        remaining -= len;
      }
      if (remaining <= 0) {
        break;
      }
      // value
      {
        auto len = strnlen(&buf_[next], remaining) + 1;
        printf("%s\n", &buf_[next]);
        next += len;
        remaining -= len;
      }
    }
  }

 private:
  ConstBuffer buf_;
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

template <typename T1, typename T2, typename L>
static void foreachPair(const T1& t1, const T2& t2, const L& fn) {
  CHECK(t1.size() == t2.size());
  for (typename T1::size_type i = 0; i < t1.size(); i++) {
    fn(t1[i], t2[i]);
  }
}

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

  // Note that this only handles uncompiled classes. Compiled classes
  // additionally contain a bitmap for each method, along with a field
  // specifying the length of the bitmap.
  struct PACKED ClassInfo {
    int16_t status;
    uint16_t type;
  };

protected:
  OatClasses() = default;
};

class DexFileListing_079;
class DexFiles;

// Note: There is no separate 0atClasses_064, since in 064 this data is
// part of DexFileListing_064.
class OatClasses_079 : public OatClasses {
public:
  struct DexClasses {
    std::string dex_file;
    std::vector<ClassInfo> class_info;
  };

  OatClasses_079(const DexFileListing_079& dex_file_listing,
                 const DexFiles& dex_files,
                 ConstBuffer oat_buf);

  MOVABLE(OatClasses_079);

  void print();

private:
  std::vector<DexClasses> classes_;
};

class DexFileListing {
public:
  struct DexFile {
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
    std::string location;
    uint32_t location_checksum;
    uint32_t file_offset;
    std::vector<ClassInfo> class_info;
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

      for (unsigned int i = 0; i < num_classes; i++) {
        uint32_t class_info_offset;
        READ_WORD(&class_info_offset, ptr);

        ClassInfo class_info;
        cur_ma()->memcpyAndMark(&class_info, oat_buf.ptr + class_info_offset,
                                sizeof(ClassInfo));
        file.class_info.push_back(class_info);
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
      for (const auto& info : e.class_info) {
        printf("    { status: %s type %s }\n",
               OatClasses::statusStr(static_cast<OatClasses::Status>(info.status)),
               OatClasses::typeStr(static_cast<OatClasses::Type>(info.type)));
      }
      printf("  }\n");
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


OatClasses_079::OatClasses_079(const DexFileListing_079& dex_file_listing,
           const DexFiles& dex_files,
           ConstBuffer oat_buf) {
  foreachPair(
      dex_file_listing.dex_files(),
      dex_files.headers(),
      [&](const DexFileListing_079::DexFile_079& listing,
          const DexFileHeader& header) {

        auto classes_offset = listing.classes_offset;

        DexClasses dex_classes;
        dex_classes.dex_file = listing.location;

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
        }
        classes_.push_back(dex_classes);
      });
}

void OatClasses_079::print() {
  for (const auto& e : classes_) {
    printf("  { Classes for dex %s\n", e.dex_file.c_str());
    for (const auto& info : e.class_info) {
      printf("    { status: %s type %s }\n",
             statusStr(static_cast<Status>(info.status)),
             typeStr(static_cast<Type>(info.type)));
    }
    printf("  }\n");
  }
}



template <typename T>
inline T nextPowerOfTwo(T in) {
  // Turn off all but msb.
  while ((in & (in - 1u)) != 0) {
    in &= in - 1u;
  }
  return in << 1u;
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
    uint32_t str_offset;
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

 private:
  static bool supportedSize(uint32_t num_class_defs) {
    return num_class_defs != 0u &&
           num_class_defs <= std::numeric_limits<uint16_t>::max();
  }

  static uint32_t numEntries(uint32_t num_classes) {
    return supportedSize(num_classes) ? nextPowerOfTwo(num_classes) : 0u;
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

  void print(bool dump_classes, bool dump_tables) override {
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

  void print(bool dump_classes, bool dump_tables) override {
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
  }

  Status status() override { return Status::PARSE_SUCCESS; }

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
  void print(bool dump_classes, bool dump_tables) override {
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
  void print(bool dump_classes, bool dump_tables) override {
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
