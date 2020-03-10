/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexDefs.h"
#include "OatmealUtil.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

constexpr uint32_t kOatMagicNum = 0x0a74616F;

enum class OatVersion : uint32_t {
  UNKNOWN = 0,
  V_039 = 0x00393330, // 5.0, api level 21
  V_045 = 0x00353430, // 5.1, api level 22
  V_064 = 0x00343630, // 6.0, api level 23
  V_067 = 0x00373630, // 6.0, api level 23
  V_079 = 0x00393730, // 7.0, api level 24
  V_088 = 0x00383830, // 7.1, api level 25
  V_124 = 0x00343231, // 8.0, api level 26
  V_131 = 0x00313331 // 8.1, api level 27
};

enum class VdexVersion : uint32_t {
  UNKNOWN = 0,
  V_006 = 0x00363030, // 8.0, api level 26
  V_010 = 0x00303130 // 8.1, api level 27
};

struct DexInput {
  std::string filename; // the location on disk.
  std::string location; // the name to store in the OAT file.
};

struct OatDexFile {
  OatDexFile() = default;
  OatDexFile(std::string location_, uint32_t file_offset_, uint32_t file_size_)
      : location(std::move(location_)),
        file_offset(file_offset_),
        file_size(file_size_) {}

  std::string location;
  uint32_t file_offset;
  uint32_t file_size;
};

class OatFile {
 public:
  enum class Status {
    PARSE_SUCCESS,
    PARSE_UNKNOWN_VERSION,
    PARSE_BAD_MAGIC_NUMBER,
    PARSE_FAILURE,

    BUILD_SUCCESS,
    BUILD_UNSUPPORTED_VERSION,
    BUILD_IO_ERROR,
    BUILD_ARG_ERROR
  };

  OatFile() = default;

  UNCOPYABLE(OatFile);
  MOVABLE(OatFile);
  virtual ~OatFile();

  // Reads magic number, returns correct oat file implementation.
  static std::unique_ptr<OatFile> parse(ConstBuffer oatfile_buffer,
                                        const std::vector<DexInput>& dexes,
                                        bool dex_files_only);

  // Like parse, but stops after parsing the dex file listing and dex headers.
  static std::unique_ptr<OatFile> parse_dex_files_only(ConstBuffer buf);
  static std::unique_ptr<OatFile> parse_dex_files_only(void* ptr, size_t len);

  virtual std::vector<OatDexFile> get_oat_dexfiles() = 0;

  virtual void print(bool dump_classes,
                     bool dump_tables,
                     bool print_unverified_classes) = 0;

  virtual Status status() = 0;

  // Return the version number as a string, e.g. "039", "079", etc.
  virtual std::string version_string() const = 0;

  virtual bool created_by_oatmeal() const = 0;

  // In an OatFile created with parse or parse_dex_files_only, the buffer may
  // have been an ELF file, in which case the parsing starts at the offset of
  // the ELF file's .rodata section. This returns the offset to that data. (Or
  // zero if the buffer was not an elf file.)
  virtual size_t oat_offset() const = 0;

  // Return true if we've detected samsung customizations to the oatfile format.
  virtual bool is_samsung() const = 0;

  // Return the location of the art boot image, or null if there is none.
  virtual std::unique_ptr<std::string> get_art_image_loc() const = 0;

  static Status build(const std::vector<std::string>& oat_files,
                      const std::vector<DexInput>& dex_files,
                      const std::string& oat_version,
                      const std::string& arch,
                      bool write_elf,
                      const std::string& art_image_location,
                      bool samsung_mode,
                      const std::string& quick_data_location);
};

enum class InstructionSet {
  kNone = 0,
  kArm = 1,
  kArm64 = 2,
  kThumb2 = 3,
  kX86 = 4,
  kX86_64 = 5,
  kMips = 6,
  kMips64 = 7,
  kMax
};

struct ArchStrings {
  InstructionSet i;
  const char* s;
};

constexpr ArchStrings arch_strings[] = {
    {InstructionSet::kNone, "NONE"},   {InstructionSet::kArm, "arm"},
    {InstructionSet::kArm64, "arm64"}, {InstructionSet::kThumb2, "thumb2"},
    {InstructionSet::kX86, "x86"},     {InstructionSet::kX86_64, "x86_64"},
    {InstructionSet::kMips, "mips"},   {InstructionSet::kMips64, "mips64"},
    {InstructionSet::kMax, nullptr}};

inline const char* instruction_set_str(InstructionSet isa) {
  int i = 0;
  while (arch_strings[i].i != InstructionSet::kMax) {
    if (arch_strings[i].i == isa) {
      return arch_strings[i].s;
    }
    i++;
  }
  return "<UNKNOWN>";
}

inline InstructionSet instruction_set(const std::string& isa) {
  int i = 0;
  while (arch_strings[i].i != InstructionSet::kMax) {
    if (isa == arch_strings[i].s) {
      return arch_strings[i].i;
    }
    i++;
  }
  return InstructionSet::kMax;
}

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
