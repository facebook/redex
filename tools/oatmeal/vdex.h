/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "OatmealUtil.h"
#include "dex.h"
#include "memory-accounter.h"

constexpr uint32_t kVdexMagicNum = 0x78656476;

using VdexChecksum = uint32_t;

struct PACK VdexFileHeader {
  uint8_t magic_[4];
  uint8_t version_[4];
  uint32_t number_of_dex_files_;
  uint32_t dex_size_;
  uint32_t verifier_deps_size_;
  uint32_t quickening_info_size_;

  static VdexFileHeader parse(ConstBuffer buf) {
    VdexFileHeader header;
    CHECK(buf.len >= sizeof(VdexFileHeader));
    cur_ma()->memcpyAndMark(&header, buf.ptr, sizeof(VdexFileHeader));
    return header;
  }

  void print() const {
    // version_ has a newline character at idx 4.
    printf(
        "VdexFileHeader: {magic: 0x%08x, \
      version: %s, \
      dex_files_no: 0x%08x (%u), \
      dex_size_: 0x%08x (%u), \
      verifier_deps_size_: 0x%08x (%u), \
      quickening_info_size_: 0x%08x (%u)}\n",
        *(reinterpret_cast<const uint32_t*>(magic_)),
        reinterpret_cast<const char*>(version_), number_of_dex_files_,
        number_of_dex_files_, dex_size_, dex_size_, verifier_deps_size_,
        verifier_deps_size_, quickening_info_size_, quickening_info_size_);
  }
};

/*
VDEX files contain extracted/quickened DEX files in 8.0>

File format:
   VdexFileHeader    fixed-length header
   Checksum[0]
   ...
   Checksum[D]
   DEX[0]            array of the input DEX files
   ...
   DEX[D]
*/
class VdexFile {
 public:
  UNCOPYABLE(VdexFile);
  MOVABLE(VdexFile);

  static std::unique_ptr<VdexFile> parse(ConstBuffer buf);
  void print() const;

 private:
  VdexFile(VdexFileHeader& header, ConstBuffer buf);

  VdexFileHeader header_;
  std::vector<DexFileHeader> dex_headers_;
  std::vector<ConstBuffer> dexes_;
};
