/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

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
#include "elf-writer.h"

const std::string& ElfStringTable::at(int orig_idx) const {
  auto idx = orig_idx;
  for (const auto& str : strings_) {
    if (idx < 0) {
      fprintf(stderr,
              "warning: invalid index %d into elf string table of size %zu\n",
              orig_idx,
              strings_.size());
    }
    if (idx <= 0) {
      return str;
    }
    idx -= str.size() + 1;
  }
  fprintf(stderr,
          "warning: invalid index %d into elf string table of size %zu\n",
          orig_idx,
          strings_.size());
  return strings_[0];
}

void ElfWriter::build(InstructionSet isa,
                      Elf32_Word oat_size,
                      Elf32_Word bss_size) {

  elf_header_.e_ident[0] = 0x7f;
  elf_header_.e_ident[1] = 'E';
  elf_header_.e_ident[2] = 'L';
  elf_header_.e_ident[3] = 'F';

  elf_header_.e_ident[4] = ELFCLASS32;
  elf_header_.e_ident[5] = ELFDATA2LSB;
  elf_header_.e_ident[6] = EV_CURRENT;
  elf_header_.e_ident[7] = ELFOSABI_LINUX;
  elf_header_.e_ident[8] = 0;

  elf_header_.e_type = ET_DYN;
  switch (isa) {
  case InstructionSet::kArm:
    elf_header_.e_machine = EM_ARM;
    elf_header_.e_flags = 0x5000000;
    break;
  case InstructionSet::kX86:
    elf_header_.e_machine = EM_386;
    elf_header_.e_flags = 0;
    break;
  default:
    CHECK(false, "Unsupported architecture %s", instruction_set_str(isa));
    break;
  }

  elf_header_.e_version = EV_CURRENT;
  elf_header_.e_entry = 0;

  static_assert(sizeof(Elf32_Ehdr) == 52, "unexpected elf header size");
  elf_header_.e_phoff = sizeof(Elf32_Ehdr);

  elf_header_.e_ehsize = sizeof(Elf32_Ehdr);

  // Put empty string at start of string_table_ to match ART's convention.
  string_table_.get_string("");

  build_dynstr_table();

  switch (oat_version_) {
  case OatVersion::V_039:
  case OatVersion::V_045:
  case OatVersion::V_064:
  case OatVersion::V_067:
    next_offset_ = 0x134;
    next_addr_ = 0x134;

    add_empty_section_header();
    add_dynsym();
    add_dynstr();
    add_hash();
    add_rodata(oat_size);
    add_text();
    add_dynamic();
    add_shstrtab();
    break;

  case OatVersion::V_079:
  case OatVersion::V_088:
    // First 4k is reserved for ELF header and program headers.
    next_offset_ = 0x1000;
    next_addr_ = 0x1000;

    add_empty_section_header();
    add_rodata(oat_size);
    add_text();
    add_bss(bss_size);
    add_dynstr();
    add_dynsym();
    add_hash();
    add_dynamic();
    add_shstrtab();
    break;

  case OatVersion::V_124:
  case OatVersion::V_131:
    // First 4k is reserved for ELF header and program headers.
    next_offset_ = 0x1000;
    next_addr_ = 0x1000;

    add_empty_section_header();
    add_rodata(oat_size);
    add_text();
    add_dynstr();
    add_dynsym();
    add_hash();
    add_dynamic();
    add_shstrtab();
    break;

  case OatVersion::UNKNOWN:
    CHECK(false,
          "Illegal OatVersion 0x%08x",
          static_cast<uint32_t>(oat_version_));
    break;
  }

  link_section(hash_idx_, dynsym_idx_);
  link_section(dynsym_idx_, dynstr_idx_);
  link_section(dynamic_idx_, dynstr_idx_);

  elf_header_.e_shentsize = sizeof(Elf32_Shdr);
  elf_header_.e_shnum = section_headers_.size();
  elf_header_.e_shstrndx = shstrtab_idx_;
}

void ElfWriter::write(FileHandle& fh) {
  write_dynstr(fh);
  write_dynsym(fh);
  write_hash(fh);
  write_dynamic(fh);
  write_shstrtab(fh);
  write_headers(fh);
  write_program_headers(fh);

  fh.seek_begin();
  write_obj(fh, elf_header_);
}

unsigned int ElfWriter::get_num_dynsymbols() const {
  switch (oat_version_) {
  case OatVersion::V_039:
  case OatVersion::V_045:
  case OatVersion::V_064:
  case OatVersion::V_067:
    // There are 4 symbols in the dynsym section (oatbss and oatbsslastword
    // would be added if we were generating a bss section, but we aren't):
    // 0: Empty
    // 1: oatdata
    // 2: oatexec
    // 3: oatlastword
    return 4;

  case OatVersion::V_079:
  case OatVersion::V_088:
    // There are 5 symbols in the dynsym section:
    // 0: Empty
    // 1: oatdata
    // 2: oatlastword
    // 3: oatbss
    // 4: oatbsslastword
    return 5;

  case OatVersion::V_124:
  case OatVersion::V_131:
    // There are 3 symbols in the dynsym section:
    // 0: Empty
    // 1: oatdata
    // 2: oatlastword
    return 3;

  case OatVersion::UNKNOWN:
    break;
  }
  CHECK(
      false, "Illegal OatVersion 0x%08x", static_cast<uint32_t>(oat_version_));
  return 0;
}

void ElfWriter::build_dynstr_table() {
  dynstr_table_.get_string("");
  dynstr_table_.get_string("oatdata");
  if (oat_version_ == OatVersion::V_039 || oat_version_ == OatVersion::V_045 ||
      oat_version_ == OatVersion::V_064 || oat_version_ == OatVersion::V_067) {
    dynstr_table_.get_string("oatexec");
  }
  dynstr_table_.get_string("oatlastword");
  if (oat_version_ == OatVersion::V_079 || oat_version_ == OatVersion::V_088) {
    dynstr_table_.get_string("oatbss");
    dynstr_table_.get_string("oatbsslastword");
  }
  // TODO: probably want the real name here.
  dynstr_table_.get_string("everything.oat");
  dynstr_table_.finalize();
}

void ElfWriter::add_empty_section_header() {
  section_headers_.push_back(Elf32_Shdr{});
}

void ElfWriter::add_rodata(Elf32_Word oat_size) {
  constexpr int kAlign = 0x1000;
  next_addr_ = align<kAlign>(next_addr_);
  next_offset_ = align<kAlign>(next_offset_);

  rodata_idx_ = add_section_header(string_table_.get_string(".rodata"),
                                   SHT_PROGBITS,
                                   SHF_ALLOC,
                                   next_addr_,
                                   next_offset_,
                                   oat_size,
                                   0,
                                   0,
                                   kAlign,
                                   0);
  next_offset_ += oat_size;
  next_addr_ += oat_size;
}

void ElfWriter::add_text() {
  constexpr int kAlign = 0x1000;
  next_addr_ = align<kAlign>(next_addr_);
  next_offset_ = align<kAlign>(next_offset_);

  text_idx_ = add_section_header(string_table_.get_string(".text"),
                                 SHT_PROGBITS,
                                 SHF_ALLOC | SHF_EXECINSTR,
                                 next_addr_,
                                 next_offset_,
                                 0,
                                 0,
                                 0,
                                 kAlign,
                                 0);
}

void ElfWriter::add_bss(Elf32_Word bss_size) {
  constexpr int kAlign = 0x1000;
  next_addr_ = align<kAlign>(next_addr_);
  next_offset_ = align<kAlign>(next_offset_);

  // ALL ABOUT DA BSS
  bss_idx_ = add_section_header(string_table_.get_string(".bss"),
                                SHT_NOBITS,
                                SHF_ALLOC,
                                next_addr_,
                                0, // no offset, bss isn't in the file.
                                bss_size,
                                0,
                                0,
                                kAlign,
                                0);
  next_addr_ += bss_size;
  // no offset adjustement
}

static int get_strtab_alignment(OatVersion version) {
  switch (version) {
  case OatVersion::V_039:
  case OatVersion::V_045:
  case OatVersion::V_064:
  case OatVersion::V_067:
    return 1;
  case OatVersion::V_079:
  case OatVersion::V_088:
  case OatVersion::V_124:
  case OatVersion::V_131:
    return 0x1000;
  case OatVersion::UNKNOWN:
  default: {
    fprintf(stderr, "version 0x%08x unknown\n", static_cast<int>(version));
    return 1;
  }
  }
}

static Elf32_Word get_str_entsize(OatVersion version) {
  switch (version) {
  case OatVersion::V_039:
  case OatVersion::V_045:
    return 1;
  case OatVersion::V_064:
  case OatVersion::V_067:
  case OatVersion::V_079:
  case OatVersion::V_088:
  case OatVersion::V_124:
  case OatVersion::V_131:
    return 0;
  case OatVersion::UNKNOWN:
  default: {
    fprintf(stderr, "version 0x%08x unknown\n", static_cast<int>(version));
    return 0;
  }
  }
}

void ElfWriter::add_dynstr() {
  const auto alignment = get_strtab_alignment(oat_version_);
  next_addr_ = align(alignment, next_addr_);
  next_offset_ = align(alignment, next_offset_);

  auto dynstr_size = dynstr_table_.size();

  auto ent_size = get_str_entsize(oat_version_);

  dynstr_idx_ = add_section_header(string_table_.get_string(".dynstr"),
                                   SHT_STRTAB,
                                   SHF_ALLOC,
                                   next_addr_,
                                   next_offset_,
                                   dynstr_size,
                                   0,
                                   0,
                                   alignment,
                                   ent_size);
  next_addr_ += dynstr_size, next_offset_ += dynstr_size;
}

void ElfWriter::add_dynsym() {
  constexpr int kAlign = 4;
  next_addr_ = align<kAlign>(next_addr_);
  next_offset_ = align<kAlign>(next_offset_);

  auto dynsym_size = get_num_dynsymbols() * sizeof(Elf32_Sym);

  dynsym_idx_ = add_section_header(string_table_.get_string(".dynsym"),
                                   SHT_DYNSYM,
                                   SHF_ALLOC,
                                   next_addr_,
                                   next_offset_,
                                   dynsym_size,
                                   0,
                                   0,
                                   kAlign,
                                   sizeof(Elf32_Sym));

  next_addr_ += dynsym_size;
  next_offset_ += dynsym_size;
}

void ElfWriter::add_hash() {
  // TODO: 064 hash table is different!

  constexpr int kAlign = 4;
  next_addr_ = align<kAlign>(next_addr_);
  next_offset_ = align<kAlign>(next_offset_);

  // it seems that both 064 and 079/088 have a hash size of 0x20, so we don't
  // base this on get_num_dynsymbols().
  constexpr int num_hash_symbols = 5;
  // + 5 for hash table metadata.
  // - 2 because we don't need entries for first or last symbol.
  const auto hash_size = sizeof(Elf32_Word) * (num_hash_symbols + 5 - 2);

  hash_idx_ = add_section_header(string_table_.get_string(".hash"),
                                 SHT_HASH,
                                 SHF_ALLOC,
                                 next_addr_,
                                 next_offset_,
                                 hash_size,
                                 0,
                                 0,
                                 kAlign,
                                 sizeof(Elf32_Word));

  next_addr_ += hash_size;
  next_offset_ += hash_size;
}

void ElfWriter::add_dynamic() {
  constexpr int kAlign = 0x1000;
  next_addr_ = align<kAlign>(next_addr_);
  next_offset_ = align<kAlign>(next_offset_);

  const auto dynamic_size = kNumDynamics * sizeof(Elf32_Dyn);

  dynamic_idx_ = add_section_header(string_table_.get_string(".dynamic"),
                                    SHT_DYNAMIC,
                                    SHF_ALLOC,
                                    next_addr_,
                                    next_offset_,
                                    dynamic_size,
                                    0,
                                    0,
                                    kAlign,
                                    sizeof(Elf32_Dyn));

  next_addr_ += dynamic_size;
  next_offset_ += dynamic_size;
}

void ElfWriter::add_shstrtab() {
  const auto alignment = get_strtab_alignment(oat_version_);
  next_addr_ = align(alignment, next_addr_);
  next_offset_ = align(alignment, next_offset_);

  const auto strtab_label_idx = string_table_.get_string(".shstrtab");
  string_table_.finalize();
  const auto strtab_size = string_table_.size();

  auto ent_size = get_str_entsize(oat_version_);

  shstrtab_idx_ = add_section_header(strtab_label_idx,
                                     SHT_STRTAB,
                                     0,
                                     0,
                                     next_offset_,
                                     strtab_size,
                                     0,
                                     0,
                                     alignment,
                                     ent_size);

  next_offset_ += strtab_size;
}

void ElfWriter::link_section(int src_idx, int dst_idx) {
  section_headers_[src_idx].sh_link = dst_idx;
}

void ElfWriter::write_dynstr(FileHandle& fh) {
  fh.seek_set(section_headers_.at(dynstr_idx_).sh_offset);
  auto flat_dynstr = dynstr_table_.flatten();
  write_buf(fh, ConstBuffer{flat_dynstr.data(), flat_dynstr.size()});
}

void ElfWriter::write_dynsym(FileHandle& fh) {

  dynsyms_.clear();

  auto add_symbol = [&](Elf32_Word str_idx,
                        Elf32_Word val,
                        Elf32_Word size,
                        unsigned char binding,
                        unsigned char type,
                        int section_idx) {
    Elf32_Sym sym = {str_idx, val, size,
                     // this is opposite of ELF_ST_BIND and ELF_ST_TYPE
                     static_cast<unsigned char>((binding << 4) | (type & 0xf)),
                     0, // must be zero
                     static_cast<Elf32_Half>(section_idx)};

    dynsyms_.push_back(sym);
  };

  dynsyms_.push_back(Elf32_Sym{});

  const auto oat_addr = section_headers_.at(rodata_idx_).sh_addr;
  const auto oat_size = section_headers_.at(rodata_idx_).sh_size;

  const auto bss_addr = section_headers_.at(bss_idx_).sh_addr;
  const auto bss_size = section_headers_.at(bss_idx_).sh_size;

  add_symbol(dynstr_table_.get_string("oatdata"),
             oat_addr,
             oat_size,
             STB_GLOBAL,
             STT_OBJECT,
             rodata_idx_);

  if (oat_version_ == OatVersion::V_067 || oat_version_ == OatVersion::V_064 ||
      oat_version_ == OatVersion::V_045 || oat_version_ == OatVersion::V_039) {
    add_symbol(dynstr_table_.get_string("oatexec"),
               oat_addr + oat_size,
               0,
               STB_GLOBAL,
               STT_OBJECT,
               text_idx_);
  }

  add_symbol(
      dynstr_table_.get_string("oatlastword"),
      oat_addr + oat_size - 4,
      4,
      STB_GLOBAL,
      STT_OBJECT,
      (oat_version_ == OatVersion::V_064 || oat_version_ == OatVersion::V_067)
          ? text_idx_
          : rodata_idx_);

  if (oat_version_ == OatVersion::V_079 || oat_version_ == OatVersion::V_088) {
    // dex2oat on 7.0 appears to write the incorrect section index (they use
    // rodata_idx_ + 1 when the text section is empty.)
    add_symbol(dynstr_table_.get_string("oatbss"),
               bss_addr,
               bss_size,
               STB_GLOBAL,
               STT_OBJECT,
               rodata_idx_ + 1);
    add_symbol(dynstr_table_.get_string("oatbsslastword"),
               bss_addr + bss_size - 4,
               4,
               STB_GLOBAL,
               STT_OBJECT,
               rodata_idx_ + 1);
  }

  CHECK(dynsyms_.size() == get_num_dynsymbols());

  fh.seek_set(section_headers_.at(dynsym_idx_).sh_offset);
  write_vec(fh, dynsyms_);
}

// Determine the number of buckets to use for the hash table
// in 064. num_dynsymbols will always be < 8 in practice, afaict.
static uint32_t num_hash_buckets_064(uint32_t num_dynsymbols) {
  if (num_dynsymbols < 8) {
    return 2;
  } else if (num_dynsymbols < 32) {
    return 4;
  } else if (num_dynsymbols < 256) {
    return 16;
  } else {
    return roundUpToPowerOfTwo(num_dynsymbols / 32);
  }
}

uint32_t ElfWriter::hash_dynsym(int sym_idx) const {
  const auto& sym = dynsyms_.at(sym_idx);
  std::string name = dynstr_table_.at(sym.st_name);

  auto name_cstr = reinterpret_cast<const uint8_t*>(name.c_str());
  uint32_t h = 0;
  uint32_t g = 0;

  // http://androidxref.com/6.0.1_r10/xref/art/runtime/elf_file.cc#790
  while (*name_cstr) {
    h = (h << 4) + *name_cstr++;
    g = h & 0xf0000000;
    h ^= g;
    h ^= g >> 24;
  }
  return h;
}

void ElfWriter::write_hash(FileHandle& fh) {
  const auto num_dynsymbols = get_num_dynsymbols();
  std::vector<Elf32_Word> hash;

  switch (oat_version_) {
  case OatVersion::V_039:
  case OatVersion::V_045:
  case OatVersion::V_064:
  case OatVersion::V_067: {

    CHECK(dynsyms_.size() == num_dynsymbols,
          "dynsyms must be written before the hash table");

    auto num_buckets = num_hash_buckets_064(num_dynsymbols);

    // 1 is for the implicit NULL symbol.
    Elf32_Word chain_size = num_dynsymbols;
    hash.push_back(num_buckets);
    hash.push_back(chain_size);
    uint32_t bucket_offset = hash.size();
    uint32_t chain_offset = bucket_offset + num_buckets;
    hash.resize(hash.size() + num_buckets + chain_size, 0);

    Elf32_Word* buckets = hash.data() + bucket_offset;
    Elf32_Word* chain = hash.data() + chain_offset;

    // Insert the symbols into the hash table.
    // 0 indicates an empty location in the hash table. If we find that a
    // location is already occupied, the chain table is used to store the chain
    // of indices that lead to the place where the symbol is finally inserted.
    for (unsigned int i = 1; i < num_dynsymbols; i++) {
      // Add 1 since we need to have the null symbol that is not in the symbols
      // list.
      Elf32_Word index = i;
      Elf32_Word hash_val = hash_dynsym(i) % num_buckets;
      if (buckets[hash_val] == 0) {
        buckets[hash_val] = index;
      } else {
        auto chain_idx = buckets[hash_val];
        while (chain[chain_idx] != 0) {
          chain_idx = chain[chain_idx];
        }
        chain[chain_idx] = index;
      }
    }
    break;
  }
  case OatVersion::V_079:
  case OatVersion::V_088:
  case OatVersion::V_124:
  case OatVersion::V_131: {
    // Everything goes in 1 bucket, chained.
    hash.push_back(1);
    hash.push_back(num_dynsymbols); // Number of chains.
    hash.push_back(1);
    hash.push_back(0);
    for (unsigned int i = 1; i < num_dynsymbols - 1; i++) {
      hash.push_back(i + 1); // point symbol to next one.
    }
    hash.push_back(0); // Last symbol terminates the chain.
    break;
  }
  case OatVersion::UNKNOWN:
    break;
  }

  fh.seek_set(section_headers_.at(hash_idx_).sh_offset);
  write_vec(fh, hash);
}

void ElfWriter::write_dynamic(FileHandle& fh) {

  // Calculate addresses of .dynsym, .hash and .dynamic.

  const Elf32_Sword hash_addr = section_headers_.at(hash_idx_).sh_addr;
  const Elf32_Sword dynstr_addr = section_headers_.at(dynstr_idx_).sh_addr;
  const Elf32_Sword dynstr_size = section_headers_.at(dynstr_idx_).sh_size;
  const Elf32_Sword dynsym_addr = section_headers_.at(dynsym_idx_).sh_addr;

  std::vector<Elf32_Dyn> dyns = {
      {DT_HASH, {hash_addr}},
      {DT_STRTAB, {dynstr_addr}},
      {DT_SYMTAB, {dynsym_addr}},
      {DT_SYMENT, {sizeof(Elf32_Sym)}},
      {DT_STRSZ, {dynstr_size}},
      {DT_SONAME, {dynstr_table_.get_string("everything.oat")}},
      {DT_NULL, {0}},
  };

  CHECK(dyns.size() == kNumDynamics);

  fh.seek_set(section_headers_.at(dynamic_idx_).sh_offset);
  write_vec(fh, dyns);
}

void ElfWriter::write_shstrtab(FileHandle& fh) {
  fh.seek_set(section_headers_.at(shstrtab_idx_).sh_offset);
  auto flat_strtab = string_table_.flatten();
  write_buf(fh, ConstBuffer{flat_strtab.data(), flat_strtab.size()});
}

void ElfWriter::write_headers(FileHandle& fh) {
  const auto prev_offset = next_offset_;
  next_offset_ = align<4>(next_offset_);
  const auto padding = next_offset_ - prev_offset;

  fh.seek_set(prev_offset);
  char buf[4] = {};
  write_buf(fh, ConstBuffer{buf, padding});

  elf_header_.e_shoff = next_offset_;

  write_vec(fh, section_headers_);
}

unsigned int ElfWriter::get_num_program_headers() const {
  switch (oat_version_) {
  case OatVersion::V_039:
  case OatVersion::V_045:
  case OatVersion::V_064:
  case OatVersion::V_067:
    return 5;

  case OatVersion::V_079:
  case OatVersion::V_088:
  case OatVersion::V_124:
  case OatVersion::V_131:
    return 6;

  case OatVersion::UNKNOWN:
    break;
  }
  CHECK(
      false, "Illegal OatVersion 0x%08x", static_cast<uint32_t>(oat_version_));
  return 0;
}

// Write ELF program headers.
void ElfWriter::write_program_headers(FileHandle& fh) {
  auto num_prog_headers = get_num_program_headers();

  std::vector<Elf32_Phdr> prog_headers;

  // The bootstrapping program header
  prog_headers.push_back(Elf32_Phdr{
      PT_PHDR, sizeof(Elf32_Ehdr), sizeof(Elf32_Ehdr), sizeof(Elf32_Ehdr),
      static_cast<Elf32_Word>(sizeof(Elf32_Phdr) * num_prog_headers),
      static_cast<Elf32_Word>(sizeof(Elf32_Phdr) * num_prog_headers), PF_R, 4});

  // LOAD start of elf file plus rodata.
  const auto rodata_addr = section_headers_.at(rodata_idx_).sh_addr;
  const auto rodata_size = section_headers_.at(rodata_idx_).sh_size;
  const auto rodata_end = rodata_addr + rodata_size;

  prog_headers.push_back(
      Elf32_Phdr{PT_LOAD, 0, 0, 0, rodata_end, rodata_end, PF_R, 0x1000});

  switch (oat_version_) {
  case OatVersion::V_039:
  case OatVersion::V_045:
  case OatVersion::V_064:
  case OatVersion::V_067: {
    // LOAD text
    prog_headers.push_back(Elf32_Phdr{
        PT_LOAD, rodata_end, rodata_end, rodata_end, 0,
        section_headers_.at(text_idx_).sh_size, PF_R | PF_X, 0x1000});
    break;
  }
  case OatVersion::V_079:
  case OatVersion::V_088: {
    // LOAD bss
    prog_headers.push_back(Elf32_Phdr{PT_LOAD, 0, rodata_end, rodata_end, 0,
                                      section_headers_.at(bss_idx_).sh_size,
                                      PF_R | PF_W, 0x1000});
  }
  // fallthrough
  case OatVersion::V_124:
  case OatVersion::V_131: {
    // LOAD dynstr, dynsym, hash
    const auto dynstr_offset = section_headers_.at(dynstr_idx_).sh_offset;
    const auto dynstr_addr = section_headers_.at(dynstr_idx_).sh_addr;
    const auto hash_addr = section_headers_.at(hash_idx_).sh_addr;
    const auto hash_size = section_headers_.at(hash_idx_).sh_size;
    prog_headers.push_back(
        Elf32_Phdr{PT_LOAD, dynstr_offset, dynstr_addr, dynstr_addr,
                   hash_addr + hash_size - dynstr_addr,
                   hash_addr + hash_size - dynstr_addr, PF_R, 0x1000});
    break;
  }
  case OatVersion::UNKNOWN:
    break;
  }

  // LOAD and DYNAMIC dynamic
  const auto dynamic_offset = section_headers_.at(dynamic_idx_).sh_offset;
  const auto dynamic_addr = section_headers_.at(dynamic_idx_).sh_addr;
  const auto dynamic_size = section_headers_.at(dynamic_idx_).sh_size;
  prog_headers.push_back(Elf32_Phdr{PT_LOAD, dynamic_offset, dynamic_addr,
                                    dynamic_addr, dynamic_size, dynamic_size,
                                    PF_R | PF_W, 0x1000});
  prog_headers.push_back(Elf32_Phdr{PT_DYNAMIC, dynamic_offset, dynamic_addr,
                                    dynamic_addr, dynamic_size, dynamic_size,
                                    PF_R | PF_W, 0x1000});

  elf_header_.e_phentsize = sizeof(Elf32_Phdr);
  elf_header_.e_phnum = prog_headers.size();

  fh.seek_set(sizeof(Elf32_Ehdr));
  for (const auto& e : prog_headers) {
    write_obj(fh, e);
  }
}

Elf32_Word ElfWriter::add_section_header(Elf32_Word str_idx,
                                         Elf32_Word sh_type,
                                         Elf32_Word sh_flags,
                                         Elf32_Word addr,
                                         Elf32_Word offset,
                                         Elf32_Word size,
                                         Elf32_Word link,
                                         Elf32_Word info,
                                         Elf32_Word align,
                                         Elf32_Word entsize) {

  section_headers_.push_back(Elf32_Shdr{str_idx, sh_type, sh_flags, addr,
                                        offset, size, link, info, align,
                                        entsize});
  return section_headers_.size() - 1;
}
