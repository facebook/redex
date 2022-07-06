/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "OatmealUtil.h"
#include "dump-oat.h"

#include <museum/5.0.0/bionic/libc/linux/elf.h>

#include <string>
#include <vector>

// Used for both the shstrtab and dynstr section.
class ElfStringTable {
 public:
  ElfStringTable() = default;
  UNCOPYABLE(ElfStringTable);

  Elf32_Sword get_string(const std::string& str) {
    Elf32_Sword ret = 0;
    for (const auto& s : strings_) {
      if (s == str) {
        return ret;
      }
      ret += s.size() + 1;
    }
    CHECK(!finalized_);
    strings_.push_back(str);
    return ret;
  }

  std::vector<char> flatten() {
    std::vector<char> ret;
    for (const auto& s : strings_) {
      ret.insert(ret.end(), s.begin(), s.end());
      ret.push_back('\0');
    }
    return ret;
  }

  size_t size() const {
    size_t ret = 0;
    for (const auto& s : strings_) {
      ret += s.size() + 1;
    }
    return ret;
  }

  void finalize() { finalized_ = true; }

  const std::string& at(int idx) const;

 private:
  bool finalized_ = false;
  std::vector<std::string> strings_;
};

// Used for writing the ELF packaging around an ART oat file.
class ElfWriter {
 public:
  explicit ElfWriter(OatVersion oat_version) : oat_version_(oat_version) {}

  void build(InstructionSet isa, Elf32_Word oat_size, Elf32_Word bss_size);

  void write(FileHandle& fh);

 private:
  void build_dynstr_table();

  void add_empty_section_header();
  void add_rodata(Elf32_Word oat_size);
  void add_text();
  void add_bss(Elf32_Word bss_size);
  void add_dynstr();
  void add_dynsym();
  void add_hash();
  void add_dynamic();
  void add_shstrtab();

  void link_section(int src_idx, int dst_idx);

  void write_dynstr(FileHandle& fh);
  void write_dynsym(FileHandle& fh);
  void write_hash(FileHandle& fh);
  void write_dynamic(FileHandle& fh);
  void write_shstrtab(FileHandle& fh);
  void write_headers(FileHandle& fh);
  void write_program_headers(FileHandle& fh);

  uint32_t hash_dynsym(int sym_idx) const;

  Elf32_Word add_section_header(Elf32_Word str_idx,
                                Elf32_Word sh_type,
                                Elf32_Word sh_flags,
                                Elf32_Word addr,
                                Elf32_Word offset,
                                Elf32_Word size,
                                Elf32_Word link,
                                Elf32_Word info,
                                Elf32_Word align,
                                Elf32_Word entsize);

  OatVersion oat_version_;

  Elf32_Ehdr elf_header_ = {};

  ElfStringTable string_table_;
  ElfStringTable dynstr_table_;

  Elf32_Word next_offset_ = 0x1000;
  Elf32_Word next_addr_ = 0x1000;

  unsigned int get_num_dynsymbols() const;
  unsigned int get_num_program_headers() const;

  // There are 7 dynamic sections: DT_HASH, DT_STRTAB,
  // DT_SYMTAB, DT_SYMENT, DT_STRZ, DT_SONAME, and
  // a terminating DT_NULL.
  static constexpr int kNumDynamics = 7;

  // 6 program headers:
  // - metaprogram header (load program headers)
  // - load headers + .rodata
  // - load .bss
  // - load .dynstr, .dynsym, .hash
  // - load .hash
  // - dynamic .dynamic
  static constexpr int kNumProgHeaders = 6;

  int rodata_idx_ = 0;
  int text_idx_ = 0;
  int bss_idx_ = 0;
  int dynstr_idx_ = 0;
  int dynsym_idx_ = 0;
  int hash_idx_ = 0;
  int dynamic_idx_ = 0;
  int shstrtab_idx_ = 0;

  std::vector<Elf32_Shdr> section_headers_;
  std::vector<Elf32_Sym> dynsyms_;
};
