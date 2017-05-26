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
#include "util.h"

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
  elf_header_.e_ident[7] = ELFOSABI_GNU;
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

  // First 4k is reserved for ELF header and program headers.
  next_offset_ = 0x1000;
  next_addr_ = 0x1000;

  // Put empty string at start of string_table_
  string_table_.get_string("");

  add_empty_section_header();
  add_rodata(oat_size);
  add_text();
  add_bss(bss_size);
  add_dynstr();
  add_dynsym();
  add_hash();
  add_dynamic();
  add_shstrtab();

  elf_header_.e_shentsize = sizeof(Elf32_Shdr);
  elf_header_.e_shnum = section_headers.size();
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

void ElfWriter::add_empty_section_header() {
  section_headers.push_back(Elf32_Shdr {});
}

void ElfWriter::add_rodata(Elf32_Word oat_size) {
  constexpr int kAlign = 0x1000;
  next_addr_ = align<kAlign>(next_addr_);
  next_offset_ = align<kAlign>(next_offset_);

  rodata_idx_ = add_section_header(
      string_table_.get_string(".rodata"),
      SHT_PROGBITS,
      SHF_ALLOC,
      next_addr_,
      next_offset_,
      oat_size,
      0, 0, kAlign, 0);
  next_offset_ += oat_size;
  next_addr_ += oat_size;
}

void ElfWriter::add_text() {
  constexpr int kAlign = 0x1000;
  next_addr_ = align<kAlign>(next_addr_);
  next_offset_ = align<kAlign>(next_offset_);

  text_idx_ = add_section_header(
    string_table_.get_string(".text"),
    SHT_PROGBITS,
    SHF_ALLOC | SHF_EXECINSTR,
    next_addr_,
    next_offset_,
    0,
    0, 0, kAlign, 0
  );
}

void ElfWriter::add_bss(Elf32_Word bss_size) {
  constexpr int kAlign = 0x1000;
  next_addr_ = align<kAlign>(next_addr_);
  next_offset_ = align<kAlign>(next_offset_);

  // ALL ABOUT DA BSS
  bss_idx_ = add_section_header(
    string_table_.get_string(".bss"),
    SHT_NOBITS,
    SHF_ALLOC,
    next_addr_,
    0, // no offset, bss isn't in the file.
    bss_size,
    0, 0, kAlign, 0
  );
  next_addr_ += bss_size;
  // no offset adjustement
}

void ElfWriter::add_dynstr() {
  constexpr int kAlign = 0x1000;
  next_addr_ = align<kAlign>(next_addr_);
  next_offset_ = align<kAlign>(next_offset_);

  dynstr_table_.get_string("");
  dynstr_table_.get_string("oatdata");
  dynstr_table_.get_string("oatlastword");
  dynstr_table_.get_string("oatbss");
  dynstr_table_.get_string("oatbsslastword");
  dynstr_table_.get_string("everything.oat");
  dynstr_table_.finalize();

  auto dynstr_size = dynstr_table_.size();

  dynstr_idx_ = add_section_header(
    string_table_.get_string(".dynstr"),
    SHT_STRTAB,
    SHF_ALLOC,
    next_addr_,
    next_offset_,
    dynstr_size,
    0, 0, kAlign, 0
  );
  next_addr_ += dynstr_size,
  next_offset_ += dynstr_size;
}

void ElfWriter::add_dynsym() {
  constexpr int kAlign = 4;
  next_addr_ = align<kAlign>(next_addr_);
  next_offset_ = align<kAlign>(next_offset_);

  auto dynsym_size = kNumDynSymbols * sizeof(Elf32_Sym);

  dynsym_idx_ = add_section_header(
    string_table_.get_string(".dynsym"),
    SHT_DYNSYM,
    SHF_ALLOC,
    next_addr_,
    next_offset_,
    dynsym_size,
    // is link always 4?
    4, 0, kAlign, sizeof(Elf32_Sym)
  );

  next_addr_ += dynsym_size;
  next_offset_ += dynsym_size;
}

void ElfWriter::add_hash() {
  constexpr int kAlign = 4;
  next_addr_ = align<kAlign>(next_addr_);
  next_offset_ = align<kAlign>(next_offset_);

  // + 5 for hash table metadata.
  // - 2 because we don't need entries for first or last symbol.
  const auto hash_size = sizeof(Elf32_Word) * (kNumDynSymbols + 5 - 2);

  hash_idx_ = add_section_header(
    string_table_.get_string(".hash"),
    SHT_HASH,
    SHF_ALLOC,
    next_addr_,
    next_offset_,
    hash_size,
    // is link always 5?
    5, 0, kAlign, sizeof(Elf32_Word)
  );

  next_addr_ += hash_size;
  next_offset_ += hash_size;
}

void ElfWriter::add_dynamic() {
  constexpr int kAlign = 0x1000;
  next_addr_ = align<kAlign>(next_addr_);
  next_offset_ = align<kAlign>(next_offset_);

  const auto dynamic_size = kNumDynamics * sizeof(Elf32_Dyn);

  dynamic_idx_ = add_section_header(
    string_table_.get_string(".dynamic"),
    SHT_DYNAMIC,
    SHF_ALLOC,
    next_addr_,
    next_offset_,
    dynamic_size,
    // is link always 4?
    4, 0, kAlign, sizeof(Elf32_Dyn)
  );

  next_addr_ += dynamic_size;
  next_offset_ += dynamic_size;
}

void ElfWriter::add_shstrtab() {
  constexpr int kAlign = 0x1000;
  next_addr_ = align<kAlign>(next_addr_);
  next_offset_ = align<kAlign>(next_offset_);

  const auto strtab_label_idx = string_table_.get_string(".shstrtab");
  string_table_.finalize();
  const auto strtab_size = string_table_.size();

  shstrtab_idx_ = add_section_header(
    strtab_label_idx,
    SHT_STRTAB,
    0,
    0,
    next_offset_,
    strtab_size,
    0, 0, kAlign, 0
  );

  next_offset_ += strtab_size;
}

void ElfWriter::write_dynstr(FileHandle& fh) {
  fh.seek_set(section_headers.at(dynstr_idx_).sh_offset);
  auto flat_dynstr = dynstr_table_.flatten();
  write_buf(fh, ConstBuffer { flat_dynstr.data(), flat_dynstr.size() });
}

void ElfWriter::write_dynsym(FileHandle& fh) {

  std::vector<Elf32_Sym> dynsyms;
  auto add_symbol = [&](Elf32_Word str_idx, Elf32_Word val, Elf32_Word size,
                        int binding, int type, int section_idx) {
    Elf32_Sym sym = {
      str_idx,
      val,
      size,
      0, // set after initialization.
      0, // must be zero
      static_cast<Elf32_Half>(section_idx)
    };
    sym.setBinding(binding);
    sym.setType(type);

    dynsyms.push_back(sym);
  };

  dynsyms.push_back(Elf32_Sym {});

  const auto oat_addr = section_headers.at(rodata_idx_).sh_addr;
  const auto oat_size = section_headers.at(rodata_idx_).sh_size;

  const auto bss_addr = section_headers.at(bss_idx_).sh_addr;
  const auto bss_size = section_headers.at(bss_idx_).sh_size;

  add_symbol(dynstr_table_.get_string("oatdata"),
      oat_addr, oat_size, STB_GLOBAL, STT_OBJECT, rodata_idx_);
  add_symbol(dynstr_table_.get_string("oatlastword"),
      oat_addr + oat_size - 4, 4, STB_GLOBAL, STT_OBJECT, rodata_idx_);

  // dex2oat on 7.0 appears to write the incorrect section index (they use
  // rodata_idx_ + 1 when the text section is empty.)
  add_symbol(dynstr_table_.get_string("oatbss"),
      bss_addr,
      bss_size,
      STB_GLOBAL, STT_OBJECT, rodata_idx_ + 1);
  add_symbol(dynstr_table_.get_string("oatbsslastword"),
      bss_addr + bss_size - 4, 4, STB_GLOBAL, STT_OBJECT, rodata_idx_ + 1);

  CHECK(dynsyms.size() == kNumDynSymbols);

  fh.seek_set(section_headers.at(dynsym_idx_).sh_offset);
  write_vec(fh, dynsyms);
}

void ElfWriter::write_hash(FileHandle& fh) {
  std::vector<Elf32_Word> hash;
  // Everything goes in 1 bucket, chained.
  hash.push_back(1);
  hash.push_back(kNumDynSymbols); // Number of chains.

  hash.push_back(1);
  hash.push_back(0);
  for (int i = 1; i < kNumDynSymbols - 1; i++) {
    hash.push_back(i + 1);  // point symbol to next one.
  }
  hash.push_back(0);  // Last symbol terminates the chain.

  fh.seek_set(section_headers.at(hash_idx_).sh_offset);
  write_vec(fh, hash);
}

void ElfWriter::write_dynamic(FileHandle& fh) {

  // Calculate addresses of .dynsym, .hash and .dynamic.

  const auto hash_addr = section_headers.at(hash_idx_).sh_addr;
  const auto dynstr_addr = section_headers.at(dynstr_idx_).sh_addr;
  const auto dynstr_size = section_headers.at(dynstr_idx_).sh_size;
  const auto dynsym_addr = section_headers.at(dynsym_idx_).sh_addr;

  std::vector<Elf32_Dyn> dyns = {
    { DT_HASH, { hash_addr } },
    { DT_STRTAB, { dynstr_addr } },
    { DT_SYMTAB, { dynsym_addr } },
    { DT_SYMENT, { sizeof(Elf32_Sym) } },
    { DT_STRSZ, { dynstr_size } },
    { DT_SONAME, { dynstr_table_.get_string("everything.oat") } },
    { DT_NULL, { 0 } },
  };

  CHECK(dyns.size() == kNumDynamics);

  fh.seek_set(section_headers.at(dynamic_idx_).sh_offset);
  write_vec(fh, dyns);
}

void ElfWriter::write_shstrtab(FileHandle& fh) {
  fh.seek_set(section_headers.at(shstrtab_idx_).sh_offset);
  auto flat_strtab = string_table_.flatten();
  write_buf(fh, ConstBuffer { flat_strtab.data(), flat_strtab.size() });
}

void ElfWriter::write_headers(FileHandle& fh) {
  const auto prev_offset = next_offset_;
  next_offset_ = align<4>(next_offset_);
  const auto padding = next_offset_ - prev_offset;

  fh.seek_set(prev_offset);
  char buf[4] = {};
  write_buf(fh, ConstBuffer { buf, padding });

  elf_header_.e_shoff = next_offset_;

  write_vec(fh, section_headers);
}

// Write ELF program headers.
void ElfWriter::write_program_headers(FileHandle& fh) {
  std::vector<Elf32_Phdr> prog_headers;

  // The bootstrapping program header
  prog_headers.push_back(Elf32_Phdr {
      PT_PHDR,
      sizeof(Elf32_Ehdr),
      sizeof(Elf32_Ehdr),
      sizeof(Elf32_Ehdr),
      sizeof(Elf32_Phdr) * kNumProgHeaders,
      sizeof(Elf32_Phdr) * kNumProgHeaders,
      PF_R,
      4
  });

  // LOAD start of elf file plus rodata.
  const auto rodata_addr = section_headers.at(rodata_idx_).sh_addr;
  const auto rodata_size = section_headers.at(rodata_idx_).sh_size;
  const auto rodata_end = rodata_addr + rodata_size;

  prog_headers.push_back(Elf32_Phdr {
      PT_LOAD,
      0, 0, 0,
      rodata_end,
      rodata_end,
      PF_R,
      0x1000
  });

  // LOAD bss
  prog_headers.push_back(Elf32_Phdr {
      PT_LOAD,
      0, rodata_end, rodata_end,
      0,
      section_headers.at(bss_idx_).sh_size,
      PF_R | PF_W,
      0x1000
  });

  // LOAD dynstr, dynsym, hash
  const auto dynstr_offset = section_headers.at(dynstr_idx_).sh_offset;
  const auto dynstr_addr = section_headers.at(dynstr_idx_).sh_addr;
  const auto hash_addr = section_headers.at(hash_idx_).sh_addr;
  const auto hash_size = section_headers.at(hash_idx_).sh_size;
  prog_headers.push_back(Elf32_Phdr {
      PT_LOAD,
      dynstr_offset,
      dynstr_addr,
      dynstr_addr,
      hash_addr + hash_size - dynstr_addr,
      hash_addr + hash_size - dynstr_addr,
      PF_R,
      0x1000
  });

  // LOAD and DYNAMIC dynamic
  const auto dynamic_offset = section_headers.at(dynamic_idx_).sh_offset;
  const auto dynamic_addr = section_headers.at(dynamic_idx_).sh_addr;
  const auto dynamic_size = section_headers.at(dynamic_idx_).sh_size;
  prog_headers.push_back(Elf32_Phdr {
      PT_LOAD,
      dynamic_offset,
      dynamic_addr,
      dynamic_addr,
      dynamic_size,
      dynamic_size,
      PF_R | PF_W,
      0x1000
  });
  prog_headers.push_back(Elf32_Phdr {
      PT_DYNAMIC,
      dynamic_offset,
      dynamic_addr,
      dynamic_addr,
      dynamic_size,
      dynamic_size,
      PF_R | PF_W,
      0x1000
  });

  elf_header_.e_phentsize = sizeof(Elf32_Phdr);
  elf_header_.e_phnum = prog_headers.size();

  fh.seek_set(sizeof(Elf32_Ehdr));
  for (const auto& e : prog_headers) {
    write_obj(fh, e);
  }
}

Elf32_Word ElfWriter::add_section_header(
  Elf32_Word str_idx,
  Elf32_Word sh_type,
  Elf32_Word sh_flags,
  Elf32_Word addr,
  Elf32_Word offset,
  Elf32_Word size,
  Elf32_Word link,
  Elf32_Word info,
  Elf32_Word align,
  Elf32_Word entsize) {

  section_headers.push_back(Elf32_Shdr {
    str_idx,
    sh_type,
    sh_flags,
    addr,
    offset,
    size,
    link,
    info,
    align,
    entsize
  });
  return section_headers.size() - 1;
}
