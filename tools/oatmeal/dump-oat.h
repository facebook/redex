/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "util.h"

#include <memory>
#include <vector>
#include <string>

struct DexInput {
  std::string filename; // the location on disk.
  std::string location; // the name to store in the OAT file.
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
  };

  OatFile() = default;

  UNCOPYABLE(OatFile);
  virtual ~OatFile();

  // reads magic number, returns correct oat file implementation.
  static std::unique_ptr<OatFile> parse(ConstBuffer buf);

  virtual void print(bool dump_classes, bool dump_tables, bool print_unverified_classes) = 0;

  virtual Status status() = 0;

  static Status build(const std::string& oat_file,
                      const std::vector<DexInput>& dex_files,
                      const std::string& oat_version,
                      const std::string& arch,
                      bool write_elf);
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
  { InstructionSet::kNone, "NONE" },
  { InstructionSet::kArm, "arm" },
  { InstructionSet::kArm64, "arm64" },
  { InstructionSet::kThumb2, "thumb2" },
  { InstructionSet::kX86, "x86" },
  { InstructionSet::kX86_64, "x86_64" },
  { InstructionSet::kMips, "mips" },
  { InstructionSet::kMips64, "mips64" },
  { InstructionSet::kMax, nullptr }
};

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

