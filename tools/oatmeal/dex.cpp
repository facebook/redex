/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "dex.h"
#include "DexDefs.h"
#include "DexEncoding.h"
#include "DexOpcodeDefs.h"
#include "OatmealUtil.h"
#include "QuickData.h"
#include "mmap.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <memory>
#include <unordered_map>

#define WRITE16_TO_BUFFER(buffer, to_write, file_ptr) \
  buffer << to_write;                                 \
  file_ptr += 2;

namespace {
using InsnWalkerFn =
    const std::function<void(DexOpcode, const uint16_t* const ptr)>&;
using CodeItemWalkerFn =
    const std::function<void(const uint8_t* const code_item)>&;

void make_instruction(const uint16_t** insns_ptr,
                      const QuickData* quick_data,
                      const std::string* dex,
                      size_t& file_ptr,
                      WritableBuffer& out_buffer) {
  auto& insns = *insns_ptr;
  auto fopcode = static_cast<DexOpcode>(*insns++);
  DexOpcode opcode = static_cast<DexOpcode>(fopcode & 0xff);

  // clang-format off

#ifdef DEBUG_LOG
  printf("Processing FOPCODE::OPCODE: %04x :: %02x :: %s\n",
         fopcode,
         opcode,
         print(opcode).c_str());
#endif

  switch (opcode) {
  case DOPCODE_NOP: {
#ifdef DEBUG_LOG
    printf("Processing FOPCODE: %s\n", print(fopcode).c_str());
#endif
    if (fopcode == FOPCODE_PACKED_SWITCH) {
      size_t count = (*insns--) * 2 + 4;
      for (size_t i = 0; i < count; i++) {
        WRITE16_TO_BUFFER(out_buffer, insns, file_ptr)
        insns++;
      }
      return;
    } else if (fopcode == FOPCODE_SPARSE_SWITCH) {
      size_t count = (*insns--) * 4 + 2;
      for (size_t i = 0; i < count; i++) {
        WRITE16_TO_BUFFER(out_buffer, insns, file_ptr)
        insns++;
      }
      return;
    } else if (fopcode == FOPCODE_FILLED_ARRAY) {
      uint16_t ewidth = *insns++;
      uint32_t size = *((uint32_t*)insns);
      size_t count = (ewidth * size + 1) / 2 + 4;
      insns -= 2;
      for (size_t i = 0; i < count; i++) {
        WRITE16_TO_BUFFER(out_buffer, insns, file_ptr)
        insns++;
      }
      return;
    }
  }

    /* fall through for NOP */
    SWITCH_FORMAT_10 {
      WRITE16_TO_BUFFER(out_buffer, fopcode, file_ptr)

#ifdef DEBUG_LOG
      printf("Writing OPCODE: %02x :: %s\n", opcode, print(opcode).c_str());
#endif
      break;
    }

    SWITCH_FORMAT_20 {
      WRITE16_TO_BUFFER(out_buffer, fopcode, file_ptr)

      uint16_t arg = *insns++;
      WRITE16_TO_BUFFER(out_buffer, arg, file_ptr)

#ifdef DEBUG_LOG
      printf("Writing OPCODE: %02x %02x :: %s\n",
             opcode,
             arg,
             print(opcode).c_str());
#endif
      break;
    }

    SWITCH_FORMAT_30 {
      WRITE16_TO_BUFFER(out_buffer, fopcode, file_ptr)

      uint16_t arg_low = *insns++;
      WRITE16_TO_BUFFER(out_buffer, arg_low, file_ptr)

      uint16_t arg_high = *insns++;
      WRITE16_TO_BUFFER(out_buffer, arg_high, file_ptr)

#ifdef DEBUG_LOG
      printf("Writing OPCODE: %02x %02x%02x :: %s\n",
             opcode,
             arg_low,
             arg_high,
             print(opcode).c_str());
#endif
      break;
    }

    SWITCH_FORMAT_50 {
      WRITE16_TO_BUFFER(out_buffer, fopcode, file_ptr)

      uint16_t arg_0 = *insns++;
      WRITE16_TO_BUFFER(out_buffer, arg_0, file_ptr)

      uint16_t arg_1 = *insns++;
      WRITE16_TO_BUFFER(out_buffer, arg_1, file_ptr)

      uint16_t arg_2 = *insns++;
      WRITE16_TO_BUFFER(out_buffer, arg_2, file_ptr)

      uint16_t arg_3 = *insns++;
      WRITE16_TO_BUFFER(out_buffer, arg_3, file_ptr)

#ifdef DEBUG_LOG
      printf("Writing OPCODE: %02x %02x%02x%02x%02x :: %s\n",
             opcode,
             arg_0,
             arg_1,
             arg_2,
             arg_3,
             print(opcode).c_str());
#endif
      break;
      // return new DexInstruction(insns - 5, 4);
  }

  SWITCH_FORMAT_REGULAR_FIELD_REF {
    uint16_t fidx = *insns++;

    uint16_t quick_fopcode = fopcode;
    uint16_t quick_arg = fidx;
    uint16_t quick_data_off = quick_data->get_field_offset(*dex, fidx);
    if (quick_data_off > 0) {
      quick_fopcode = (fopcode & 0xff00) | (quicken(opcode) & 0x00ff);
      quick_arg = quick_data_off;
#ifdef DEBUG_LOG
      printf("QUICKEN: [%s] %s :: %02x->%02x :: %02x->%02x\n",
        (*dex).c_str(),
        print(opcode).c_str(),
        fopcode,
        quick_fopcode,
        fidx,
        quick_arg);
#endif
    } else {
#ifdef DEBUG_LOG
      printf("No quick mapping for: [%s]:%u\n", (*dex).c_str(), fidx);
#endif
    }

      WRITE16_TO_BUFFER(out_buffer, quick_fopcode, file_ptr)
      WRITE16_TO_BUFFER(out_buffer, quick_arg, file_ptr)

#ifdef DEBUG_LOG
      printf("Writing OPCODE: %02x %02x :: %s\n",
             quick_fopcode,
             quick_arg,
             print(opcode).c_str());
#endif
      break;
    }

    SWITCH_FORMAT_REGULAR_METHOD_REF {
      WRITE16_TO_BUFFER(out_buffer, fopcode, file_ptr)

      uint16_t midx = *insns++;
      uint16_t arg = *insns++;

      WRITE16_TO_BUFFER(out_buffer, midx, file_ptr)
      WRITE16_TO_BUFFER(out_buffer, arg, file_ptr)

#ifdef DEBUG_LOG
      printf("Writing OPCODE: %02x %02x %02x :: %s\n",
             fopcode,
             midx,
             arg,
             print(opcode).c_str());
#endif
      break;
    }

    SWITCH_FORMAT_CONST_STRING {
      WRITE16_TO_BUFFER(out_buffer, fopcode, file_ptr)

      uint16_t sidx = *insns++;
      WRITE16_TO_BUFFER(out_buffer, sidx, file_ptr)

#ifdef DEBUG_LOG
      printf("Writing OPCODE: %02x %02x :: %s\n",
             fopcode,
             sidx,
             print(opcode).c_str());
#endif
      break;
    }

    SWITCH_FORMAT_CONST_STRING_JUMBO {
      WRITE16_TO_BUFFER(out_buffer, fopcode, file_ptr)

      uint16_t sidx_partial_low = *insns++;
      uint16_t sidx_partial_high = *insns++;

      WRITE16_TO_BUFFER(out_buffer, sidx_partial_low, file_ptr)
      WRITE16_TO_BUFFER(out_buffer, sidx_partial_high, file_ptr)

#ifdef DEBUG_LOG
      uint32_t sidx = sidx_partial_high << 16 | sidx_partial_low;
      printf("Writing OPCODE: %02x %04x :: %s\n",
             fopcode,
             sidx,
             print(opcode).c_str());
#endif
      break;
    }

    SWITCH_FORMAT_TYPE_REF {
      WRITE16_TO_BUFFER(out_buffer, fopcode, file_ptr)

      uint16_t tidx = *insns++;

      WRITE16_TO_BUFFER(out_buffer, tidx, file_ptr)

#ifdef DEBUG_LOG
      printf("Writing OPCODE: %02x %02x :: %s\n",
             fopcode,
             tidx,
             print(opcode).c_str());
#endif
      break;
    }

    SWITCH_FORMAT_FILL_ARRAY {
      WRITE16_TO_BUFFER(out_buffer, fopcode, file_ptr)

      uint16_t tidx = *insns++;
      uint16_t arg = *insns++;

      WRITE16_TO_BUFFER(out_buffer, tidx, file_ptr)
      WRITE16_TO_BUFFER(out_buffer, arg, file_ptr)

#ifdef DEBUG_LOG
      printf("Writing OPCODE: %02x %02x :: %s\n",
             fopcode,
             tidx,
             print(opcode).c_str());
#endif
      break;
    }
  default:
    fprintf(stderr, "Unknown opcode %02x\n", opcode);
    // return nullptr;
  }
  // clang-format on
}

/*
 * See class_data_item in Dex spec.
 */
void load_class_data_item(
    const uint8_t* class_data_item,
    std::unordered_map<uint32_t, uint32_t>& code_item_offset) {
  const uint8_t* encd = class_data_item;
  uint32_t sfield_count = read_uleb128(&encd);
  uint32_t ifield_count = read_uleb128(&encd);
  uint32_t dmethod_count = read_uleb128(&encd);
  uint32_t vmethod_count = read_uleb128(&encd);
  uint32_t ndex = 0;
  for (uint32_t i = 0; i < sfield_count; i++) {
    ndex += read_uleb128(&encd); // field_idx_diff
    read_uleb128(&encd); // access_flags
  }
  ndex = 0;
  for (uint32_t i = 0; i < ifield_count; i++) {
    ndex += read_uleb128(&encd); // field_idx_diff
    read_uleb128(&encd); // access_flags
  }
  ndex = 0;
  for (uint32_t i = 0; i < dmethod_count; i++) {
    ndex += read_uleb128(&encd);
    read_uleb128(&encd); // access_flags
    uint32_t code_off = read_uleb128(&encd);
    code_item_offset[code_off] = 0;
  }
  ndex = 0;
  for (uint32_t i = 0; i < vmethod_count; i++) {
    ndex += read_uleb128(&encd);
    read_uleb128(&encd); // access_flags
    uint32_t code_off = read_uleb128(&encd);
    code_item_offset[code_off] = 0;
  }
  (void)ndex;
}

/*
 * See code_item in Dex spec.
 */
void load_code_item(uint8_t* const code_item,
                    const QuickData* quick_data,
                    const std::string* dex,
                    size_t& file_ptr,
                    WritableBuffer& out_buffer) {
  const dex_code_item* code = reinterpret_cast<const dex_code_item*>(code_item);
  uint8_t* const dex_code_item_end =
      reinterpret_cast<uint8_t* const>(const_cast<dex_code_item*>(code + 1));
#ifdef DEBUG_LOG
  printf("method: %p, %u, %u, %u\n",
         (void*)code_item,
         code->registers_size,
         code->ins_size,
         code->outs_size);
#endif
  uint8_t* out_ptr = code_item;
  while (out_ptr < dex_code_item_end) {
    out_buffer << reinterpret_cast<char*>(out_ptr++);
    file_ptr++;
  }
  file_ptr--;

  uint32_t tries = code->tries_size;
  const uint16_t* cdata = reinterpret_cast<const uint16_t*>(dex_code_item_end);
  if (code->insns_size) {
    const uint16_t* const end = cdata + code->insns_size;
    while (cdata < end) {
      make_instruction(&cdata, quick_data, dex, file_ptr, out_buffer);
    }
    /*
     * Padding, see dex-spec.
     * Per my memory, there are dex-files where the padding is
     * implemented not according to spec.  Just FYI in case
     * something weird happens in the future.
     */
    if (code->insns_size & 1 && tries) cdata++;
  }
}

void process_instruction(const uint16_t** insns_ptr, InsnWalkerFn walker) {
  auto& insns = *insns_ptr;
  auto fopcode = static_cast<DexOpcode>(*insns);
  DexOpcode opcode = static_cast<DexOpcode>(fopcode & 0xff);
  // clang-format off
#ifdef DEBUG_LOG
  printf("Processing FOPCODE::OPCODE: %04x :: %02x :: %s\n",
         fopcode,
         opcode,
         print(opcode).c_str());
#endif

  switch (opcode) {
  case DOPCODE_NOP: {
#ifdef DEBUG_LOG
    printf("Processing FOPCODE: %s\n", print(fopcode).c_str());
#endif
    if (fopcode == FOPCODE_PACKED_SWITCH) {
      size_t count = (*(insns + 1)) * 2 + 4;
      for (size_t i = 0; i < count; i++) {
        insns++;
      }
      return;
    } else if (fopcode == FOPCODE_SPARSE_SWITCH) {
      size_t count = (*(insns + 1)) * 4 + 2;
      for (size_t i = 0; i < count; i++) {
        insns++;
      }
      return;
    } else if (fopcode == FOPCODE_FILLED_ARRAY) {
      uint16_t ewidth = *(insns + 1);
      uint32_t size = *(reinterpret_cast<const uint32_t*>(insns + 2));
      size_t count = (ewidth * size + 1) / 2 + 4;
      for (size_t i = 0; i < count; i++) {
        insns++;
      }
      return;
    }
  }

  SWITCH_FORMAT_10
  SWITCH_FORMAT_RETURN_VOID_NO_BARRIER {
    walker(opcode, insns++);

#ifdef DEBUG_LOG
    printf("Walking OPCODE: %02x :: %s\n", opcode, print(opcode).c_str());
#endif
    break;
  }

  SWITCH_FORMAT_20 {
    walker(opcode, insns++);

#ifdef DEBUG_LOG
    uint16_t arg = *insns;
    printf("Walking OPCODE: %02x %02x :: %s\n",
           opcode, arg, print(opcode).c_str());
#endif

    insns++;
    break;
  }

  SWITCH_FORMAT_30 {
    walker(opcode, insns++);

#ifdef DEBUG_LOG
    uint16_t arg_low = *insns;
    uint16_t arg_high = *(insns+1);
    printf("Walking OPCODE: %02x %02x%02x :: %s\n",
           opcode, arg_low, arg_high, print(opcode).c_str());
#endif

    insns+=2;
    break;
  }

  SWITCH_FORMAT_50 {
    walker(opcode, insns++);

#ifdef DEBUG_LOG
    uint16_t arg_0 = *insns;
    uint16_t arg_1 = *(insns+1);
    uint16_t arg_2 = *(insns+2);
    uint16_t arg_3 = *(insns+3);
    printf("Walking OPCODE: %02x %02x%02x%02x%02x :: %s\n",
           opcode, arg_0, arg_1, arg_2, arg_3, print(opcode).c_str());
#endif

    insns+=4;
    break;
  }

  SWITCH_FORMAT_REGULAR_FIELD_REF
  SWITCH_FORMAT_QUICK_FIELD_REF {
    walker(opcode, insns++);

#ifdef DEBUG_LOG
    uint16_t fidx = *insns;
    printf("Walking OPCODE: %02x %02x :: %s\n",
           fopcode, fidx, print(opcode).c_str());
#endif

    insns++;
    break;
  }

  SWITCH_FORMAT_REGULAR_METHOD_REF
  SWITCH_FORMAT_QUICK_METHOD_REF {
    walker(opcode, insns++);

#ifdef DEBUG_LOG
    uint16_t midx = *insns;
    uint16_t arg = *(insns+1);
    printf("Walking OPCODE: %02x %02x %02x :: %s\n",
           fopcode, midx, arg, print(opcode).c_str());
#endif

    insns+=2;
    break;
  }

  SWITCH_FORMAT_CONST_STRING {
    walker(opcode, insns++);

#ifdef DEBUG_LOG
    uint16_t sidx = *insns;
    printf("Walking OPCODE: %02x %02x :: %s\n",
           fopcode, sidx, print(opcode).c_str());
#endif

    insns++;
    break;
  }

  SWITCH_FORMAT_CONST_STRING_JUMBO {
    walker(opcode, insns++);

#ifdef DEBUG_LOG
    uint16_t sidx_partial_low = *insns;
    uint16_t sidx_partial_high = *(insns+1);
    uint32_t sidx = sidx_partial_high << 16 | sidx_partial_low;
    printf("Walking OPCODE: %02x %04x :: %s\n",
           fopcode, sidx, print(opcode).c_str());
#endif

    insns+=2;
    break;
  }

  SWITCH_FORMAT_TYPE_REF {
    walker(opcode, insns++);

#ifdef DEBUG_LOG
    uint16_t tidx = *insns;
    printf("Walking OPCODE: %02x %02x :: %s\n",
           fopcode, tidx, print(opcode).c_str());
#endif

    insns++;
    break;
  }

  SWITCH_FORMAT_FILL_ARRAY {
    walker(opcode, insns++);

#ifdef DEBUG_LOG
    uint16_t tidx = *insns;
    uint16_t arg = *(insns+1);
    printf("Walking OPCODE: %02x %02x %02x :: %s\n",
           fopcode, tidx, arg, print(opcode).c_str());
#endif

    insns+=2;
    break;
  }
  default:
    fprintf(stderr, "Unknown opcode %02x\n", opcode);
    // return nullptr;
  }
  // clang-format on
}

/*
 * See code_item in Dex spec.
 */
void process_code_item(const uint8_t* code_item, InsnWalkerFn walker) {
  const dex_code_item* code = reinterpret_cast<const dex_code_item*>(code_item);
  uint8_t* const dex_code_item_end =
      reinterpret_cast<uint8_t* const>(const_cast<dex_code_item*>(code + 1));
#ifdef DEBUG_LOG
  printf("method: %p, %u, %u, %u\n",
         (void*)code_item,
         code->registers_size,
         code->ins_size,
         code->outs_size);
#endif
  const uint16_t* cdata = reinterpret_cast<const uint16_t*>(dex_code_item_end);
  if (code->insns_size) {
    const uint16_t* const end = cdata + code->insns_size;
    while (cdata < end) {
      process_instruction(&cdata, walker);
    }
  }
}

} // Anonymous namespace

void quicken_dex(const char* location,
                 const QuickData* quick_data,
                 FileHandle& out) {

  FILE* fd = fopen(location, "r");
  std::string error_msg;
  CHECK(location != nullptr);
  std::unique_ptr<MappedFile> map;
  {
    START_TRACE()
    struct stat sbuf;
    memset(&sbuf, 0, sizeof(sbuf));
    if (fstat(fileno(fd), &sbuf) == -1) {
      fprintf(stderr, "DexFile: fstat '%s' failed\n", location);
      exit(1);
    }
    if (S_ISDIR(sbuf.st_mode)) {
      fprintf(stderr, "Attempt to mmap directory '%s'\n", location);
      exit(1);
    }
    size_t length = sbuf.st_size;
    map.reset(MappedFile::mmap_file(length, PROT_READ, MAP_PRIVATE, fileno(fd),
                                    location, &error_msg));
    if (map == nullptr) {
      CHECK(!error_msg.empty());
      return;
    }
    END_TRACE("mmap")
  }

  if (map->size() < sizeof(DexFileHeader)) {
    fprintf(stderr,
            "DexFile: failed to open dex file '%s' that is too short to have a "
            "header\n",
            location);
    exit(1);
  }

#ifdef DEBUG_LOG
  printf("Success: mmap() of file '%s'\n", location);
#endif

  auto dh = reinterpret_cast<const dex_header*>(map->begin());
  auto class_defs_off = dh->class_defs_off;
  std::unordered_map<uint32_t, uint32_t> class_data_offset;
  std::unordered_map<uint32_t, uint32_t> code_item_offset;
  {
    constexpr uint32_t kBufSize = 0x80000; // 1MB output buffer
    std::unique_ptr<char[]> buf(new char[kBufSize]);
    WritableBuffer out_buffer(out, buf.get(), kBufSize);
    std::string canary_name;
    for (size_t i = 0; i < map->size(); i++) {
      if (i >= class_defs_off &&
          i < class_defs_off + dh->class_defs_size * sizeof(dex_class_def) &&
          (i - class_defs_off) % sizeof(dex_class_def) == 0) {
        const dex_class_def* cdef =
            reinterpret_cast<const dex_class_def*>(map->begin() + i);

        const uint32_t* class_desc = reinterpret_cast<const uint32_t*>(
            map->begin() + dh->type_ids_off +
            cdef->typeidx * sizeof(type_id_item));
        const uint32_t* class_string_desc = reinterpret_cast<const uint32_t*>(
            map->begin() + dh->string_ids_off +
            (*class_desc) * sizeof(string_id_item));
        std::string class_name = read_string(reinterpret_cast<const uint8_t*>(
            map->begin() + (*class_string_desc)));

#ifdef DEBUG_LOG
        printf("==================\n");
        printf("Class begins at %p\n", (void*)cdef);
        printf("Class data offset: %u\n", cdef->class_data_offset);
        printf("Class: %s\n", class_name.c_str());
#endif

        std::size_t found = class_name.find("Canary");
        if (found != std::string::npos) {
#ifdef DEBUG_LOG
          printf("Found Canary Class: %s\n", class_name.c_str());
#endif
          canary_name = std::move(class_name);
        }

        if (cdef->class_data_offset) {
          load_class_data_item(map->begin() + cdef->class_data_offset,
                               code_item_offset);
        }
      }
      if (code_item_offset.count(i) != 0) {
#ifdef DEBUG_LOG
        printf("==================\n");
        printf("Code item offset: %zu\n", i);
#endif
        load_code_item(map->begin() + i, quick_data, &canary_name, i,
                       out_buffer);
      } else {
        out_buffer << reinterpret_cast<char*>(map->begin() + i);
      }
    }
  }
}

void print_dex_opcodes(const uint8_t* begin, const size_t size) {
  stream::stream_dex(
      begin,
      size,
      [](DexOpcode opcode, const uint16_t* const insn) {
        // clang-format off
        switch (opcode) {
        case DOPCODE_NOP:
        SWITCH_FORMAT_10
        SWITCH_FORMAT_RETURN_VOID_NO_BARRIER {
          printf("OPCODE: %02x :: %s :: %04x\n", opcode,
                 ::print(opcode).c_str(), *insn);
          break;
        }

        SWITCH_FORMAT_20
        SWITCH_FORMAT_REGULAR_FIELD_REF
        SWITCH_FORMAT_QUICK_FIELD_REF
        SWITCH_FORMAT_CONST_STRING
        SWITCH_FORMAT_TYPE_REF {
          printf("OPCODE: %02x :: %s :: %04x%04x\n", opcode,
                 ::print(opcode).c_str(), *insn, *(insn + 1));
          break;
        }

        SWITCH_FORMAT_30
        SWITCH_FORMAT_REGULAR_METHOD_REF
        SWITCH_FORMAT_QUICK_METHOD_REF
        SWITCH_FORMAT_CONST_STRING_JUMBO
        SWITCH_FORMAT_FILL_ARRAY {
          printf("OPCODE: %02x :: %s :: %04x%04x%04x\n", opcode,
                 ::print(opcode).c_str(), *insn, *(insn + 1), *(insn + 2));
          break;
        }

        SWITCH_FORMAT_50 {
          printf("OPCODE: %02x :: %s :: %04x%04x%04x%04x%04x\n", opcode,
                 ::print(opcode).c_str(), *insn, *(insn + 1), *(insn + 2),
                 *(insn + 3), *(insn + 4));
          break;
        }

        default: {
          fprintf(stderr, "Unknown opcode %02x\n", opcode);
        }
        }
        // clang-format on
      },
      [](const uint8_t* const insn) {});
}

void stream::stream_dex(const uint8_t* begin,
                        const size_t size,
                        InsnWalkerFn insn_walker,
                        CodeItemWalkerFn code_item_walker) {
  auto dh = reinterpret_cast<const dex_header*>(begin);
  auto class_defs_off = dh->class_defs_off;
  std::unordered_map<uint32_t, uint32_t> code_item_offset;
  {
    std::string canary_name;
    for (size_t i = 0; i < size; i++) {
      if (i >= class_defs_off &&
          i < class_defs_off + dh->class_defs_size * sizeof(dex_class_def) &&
          (i - class_defs_off) % sizeof(dex_class_def) == 0) {
        const dex_class_def* cdef =
            reinterpret_cast<const dex_class_def*>(begin + i);

        const uint32_t* class_desc = reinterpret_cast<const uint32_t*>(
            begin + dh->type_ids_off + cdef->typeidx * sizeof(type_id_item));
        const uint32_t* class_string_desc = reinterpret_cast<const uint32_t*>(
            begin + dh->string_ids_off +
            (*class_desc) * sizeof(string_id_item));
        std::string class_name = read_string(
            reinterpret_cast<const uint8_t*>(begin + (*class_string_desc)));

#ifdef DEBUG_LOG
        printf("==================\n");
        printf("Class begins at %p\n", (void*)cdef);
        printf("Class data offset: %u\n", cdef->class_data_offset);
        printf("Class: %s\n", class_name.c_str());
#endif

        std::size_t found = class_name.find("Canary");
        if (found != std::string::npos) {
#ifdef DEBUG_LOG
          printf("Found Canary Class: %s\n", class_name.c_str());
#endif
          canary_name = std::move(class_name);
        }

        load_class_data_item(begin + cdef->class_data_offset, code_item_offset);
      }
      if (code_item_offset.count(i) != 0) {
#ifdef DEBUG_LOG
        printf("==================\n");
        printf("Code item offset: %zu\n", i);
#endif
        code_item_walker(begin + i);
        process_code_item(begin + i, insn_walker);
      }
    }
  }
}
