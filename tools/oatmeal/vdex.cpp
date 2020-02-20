/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "vdex.h"

namespace {

size_t size_of_checksums_section(const VdexFileHeader& header) {
  return sizeof(VdexChecksum) * header.number_of_dex_files_;
}

} // namespace

VdexFile::VdexFile(VdexFileHeader& header, ConstBuffer buf) : header_(header) {
  auto remaining_dexes_buf =
      buf.slice(sizeof(VdexFileHeader) + size_of_checksums_section(header));
  for (size_t dex_index = 0; dex_index < header.number_of_dex_files_;
       ++dex_index) {
    auto dex_header = DexFileHeader::parse(remaining_dexes_buf);
    dex_headers_.push_back(dex_header);
    if (dex_header.magic != kDexMagicNum) {
      fprintf(stderr, "Bad dex magic\n");
      return;
    }
    printf("Version %s\n", reinterpret_cast<char*>(&(dex_header.version)));

    auto dex_buf = remaining_dexes_buf.truncate(dex_header.file_size);
    dexes_.push_back(dex_buf);
    remaining_dexes_buf = remaining_dexes_buf.slice(dex_header.file_size);
  }
}

std::unique_ptr<VdexFile> VdexFile::parse(ConstBuffer buf) {
  auto header = VdexFileHeader::parse(buf);
  header.print();

  return std::unique_ptr<VdexFile>(new VdexFile(header, buf));
}

void VdexFile::print() const {
  header_.print();
  for (const auto& e : dex_headers_) {
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
                      dex_headers_[index].file_size);
    index++;
  }
}
