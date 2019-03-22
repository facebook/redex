/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexDebugInstruction.h"
#include "DexEncoding.h"
#include "Formatters.h"
#include "PrintUtil.h"
#include "RedexDump.h"
#include "utils/Unicode.h"

#include <sstream>
#include <vector>
#include <string.h>
#include <string>

/**
 * Return a proto string in the form
 * [shorty] (argTypes)returnType
 */
static std::string get_proto(ddump_data* rd,
                             uint32_t idx,
                             bool with_shorty = true) {
  std::ostringstream ss;
  dex_proto_id* proto = rd->dex_proto_ids + idx;
  if (with_shorty) {
    ss << dex_string_by_idx(rd, proto->shortyidx) << " ";
  }
  ss << "(";
  if (proto->param_off) {
    uint32_t* tl = (uint32_t*)(rd->dexmmap + proto->param_off);
    int count = (int)*tl++;
    uint16_t* types = (uint16_t*)tl;
    for (int i = 0; i < count; i++) {
      if (i != 0) ss << " ";
      ss << dex_string_by_type_idx(rd, *types++);
    }
  }
  ss << ")" << dex_string_by_type_idx(rd, proto->rtypeidx);
  return ss.str();
}

/**
 * Return a field in the form:
 * class field_type field_name
 */
static std::string get_field(ddump_data* rd, uint32_t idx) {
  std::ostringstream ss;
  dex_field_id* field = rd->dex_field_ids + idx;
  ss << dex_string_by_type_idx(rd, field->classidx) << " "
     << dex_string_by_type_idx(rd, field->typeidx) << " "
     << dex_string_by_idx(rd, field->nameidx);
  return ss.str();
}

/**
 * Return a method in the form:
 * class method_proto_no_shorty method_name
 */
static std::string get_method(ddump_data* rd, uint32_t idx) {
  std::ostringstream ss;
  dex_method_id* method = rd->dex_method_ids + idx;
  ss << dex_string_by_type_idx(rd, method->classidx) << " "
     << dex_string_by_idx(rd, method->nameidx) << " "
     << get_proto(rd, method->protoidx, false);
  return ss.str();
}

/**
 * Return flags.
 */
static std::string get_flags(uint32_t flags,
                             bool cls = true,
                             bool method = false) {
  std::ostringstream ss;
  if (flags & DexAccessFlags::ACC_PUBLIC) {
    ss << "public ";
  }
  if (flags & DexAccessFlags::ACC_PRIVATE) {
    ss << "private ";
  }
  if (flags & DexAccessFlags::ACC_PROTECTED) {
    ss << "protected ";
  }
  if (flags & DexAccessFlags::ACC_STATIC) {
    ss << "static ";
  }
  if (flags & DexAccessFlags::ACC_FINAL) {
    ss << "final ";
  }
  if (flags & DexAccessFlags::ACC_INTERFACE) {
    ss << "interface ";
  } else if (flags & DexAccessFlags::ACC_ABSTRACT) {
    ss << "abstract ";
  }
  if (flags & DexAccessFlags::ACC_ENUM) {
    ss << "enum ";
  }
  if (flags & DexAccessFlags::ACC_SYNCHRONIZED) {
    ss << "synchronized ";
  }
  if (flags & DexAccessFlags::ACC_VOLATILE) {
    ss << (cls || method ? "bridge " : "volatile ");
  }
  if (flags & DexAccessFlags::ACC_NATIVE) {
    ss << "native ";
  }
  if (flags & DexAccessFlags::ACC_TRANSIENT) {
    ss << (method ? "varargs " : "transient ");
  }
  if (flags & DexAccessFlags::ACC_SYNTHETIC) {
    ss << "synthetic ";
  }
  return ss.str();
}

/**
 * Return a class def in the form:
 * flags class 'extends' superclass ['implements' interfaces]
 *    [file: <filename>, anno: annotation_off, data: class_data_off, static
 * values: static_value_off]
 */
static std::string get_class_def(ddump_data* rd,
                                 uint32_t idx,
                                 bool metadata = true) {
  std::ostringstream ss;
  dex_class_def* cls_def = rd->dex_class_defs + idx;
  ss << get_flags(cls_def->access_flags)
     << dex_string_by_type_idx(rd, cls_def->typeidx);
  if (cls_def->super_idx != DEX_NO_INDEX) {
    ss << " extends " << dex_string_by_type_idx(rd, cls_def->super_idx);
  }
  if (cls_def->interfaces_off) {
    ss << " implements ";
    auto interfaces = (uint32_t*)(rd->dexmmap + cls_def->interfaces_off);
    auto size = *interfaces++;
    auto types = (uint16_t*)interfaces;
    for (uint32_t i = 0; i < size; i++) {
      ss << dex_string_by_type_idx(rd, *types++);
    }
  }
  if (metadata) {
    ss << "\n\t";
    if (cls_def->source_file_idx != DEX_NO_INDEX) {
      ss << "file: " << dex_string_by_idx(rd, cls_def->source_file_idx);
    } else {
      ss << "<no_file>";
    }
    if (cls_def->annotations_off) {
      ss << ", anno: "
         << "0x" << std::hex << cls_def->annotations_off;
    }
    ss << ", data: "
       << "0x" << std::hex << cls_def->class_data_offset;
    if (cls_def->static_values_off) {
      ss << ", static values: "
         << "0x" << std::hex << cls_def->static_values_off;
    }
  }
  return ss.str();
}

/**
 * Return a class data items in the form:
 * class
 * sfields: <count>
 * 'field'
 * ...
 * ifields: <count>
 * 'field'
 * ...
 * dmethods: <count>
 * 'method'
 * ...
 * vmethods: <count>
 * 'method'
 * ...
 */
static std::string get_class_data_item(ddump_data* rd, uint32_t idx) {
  std::ostringstream ss;
  const dex_class_def* class_defs =
      (dex_class_def*)(rd->dexmmap + rd->dexh->class_defs_off) + idx;
  auto cls_off = class_defs->class_data_offset;
  if (!cls_off) return "";
  ss << dex_string_by_type_idx(rd, class_defs->typeidx) << "\n";
  const uint8_t* class_data =
      reinterpret_cast<const uint8_t*>(rd->dexmmap + cls_off);
  uint32_t sfield_count = read_uleb128(&class_data);
  uint32_t ifield_count = read_uleb128(&class_data);
  uint32_t dmethod_count = read_uleb128(&class_data);
  uint32_t vmethod_count = read_uleb128(&class_data);
  ss << "sfields: " << sfield_count << "\n";
  uint32_t fidx = 0;
  for (uint32_t i = 0; i < sfield_count; i++) {
    fidx += read_uleb128(&class_data);
    auto flags = read_uleb128(&class_data);
    ss << get_flags(flags, false) << get_field(rd, fidx) << "\n";
  }
  ss << "ifields: " << ifield_count << "\n";
  fidx = 0;
  for (uint32_t i = 0; i < ifield_count; i++) {
    fidx += read_uleb128(&class_data);
    auto flags = read_uleb128(&class_data);
    ss << get_flags(flags, false) << get_field(rd, fidx) << "\n";
  }
  ss << "dmethods: " << dmethod_count << "\n";
  uint32_t meth_idx = 0;
  for (uint32_t i = 0; i < dmethod_count; i++) {
    meth_idx += read_uleb128(&class_data);
    auto flags = read_uleb128(&class_data);
    auto code = read_uleb128(&class_data);
    ss << get_flags(flags, false, true) << "- " << get_method(rd, meth_idx)
       << " - "
       << "0x" << std::hex << code << "\n";
  }
  ss << "vmethods: " << vmethod_count << "\n";
  meth_idx = 0;
  for (uint32_t i = 0; i < vmethod_count; i++) {
    meth_idx += read_uleb128(&class_data);
    auto flags = read_uleb128(&class_data);
    auto code = read_uleb128(&class_data);
    ss << get_flags(flags, false, true) << "- " << get_method(rd, meth_idx)
       << " - "
       << "0x" << std::hex << code << "\n";
  }
  return ss.str();
}

static std::string get_code_item(dex_code_item** pcode_item) {
  dex_code_item* code_item = *pcode_item;
  std::ostringstream ss;
  ss << "registers_size: " << code_item->registers_size << ", "
     << "ins_size: " << code_item->ins_size << ", "
     << "outs_size: " << code_item->outs_size << ", "
     << "tries_size: " << code_item->tries_size << ", "
     << "debug_info_off: 0x" << std::hex << code_item->debug_info_off << ", "
     << "insns_size: " << std::dec << code_item->insns_size << "\n";
  const uint16_t* dexptr =
      (const uint16_t*)(code_item + 1) + code_item->insns_size;
  *pcode_item = (dex_code_item*)dexptr;
  if (code_item->tries_size) {
    if (code_item->insns_size & 1) dexptr++; // padding before tries
    const dex_tries_item* tries = (const dex_tries_item*)dexptr;
    const uint8_t* handlers = (const uint8_t*)(tries + code_item->tries_size);
    for (auto i = 0; i < code_item->tries_size; i++) {
      ss << "\tstart_addr: " << tries->start_addr
         << ", insn_count: " << tries->insn_count
         << ", handler_off: " << tries->handler_off << "\n";
      if (tries->handler_off) {
        const uint8_t* cur_handler = handlers + tries->handler_off;
        auto size = read_sleb128(&cur_handler);
        ss << "\t\t\thandlers size: " << size << ", ";
        for (auto j = 0; j < abs(size); j++) {
          auto type = read_uleb128(&cur_handler);
          auto addr = read_uleb128(&cur_handler);
          ss << "(type_idx: " << type << ", addr: " << addr << ") ";
        }
        if (size <= 0) {
          ss << ", catch_all_addr: " << read_uleb128(&cur_handler);
        }
        ss << "\n";
        if (cur_handler > (uint8_t*)*pcode_item) {
          *pcode_item = (dex_code_item*)cur_handler;
        }
      }
      tries++;
    }
  }
  *pcode_item = (dex_code_item*)(((uintptr_t)*pcode_item + 3) & ~3);
  return ss.str();
}

class DexDebugInstructionReader {
 protected:
  virtual void handle_advance_pc(DexDebugItemOpcode op, uint32_t arg) {
    return handle_default(op);
  }
  virtual void handle_advance_line(DexDebugItemOpcode op, int32_t arg) {
    return handle_default(op);
  }
  virtual void handle_start_local(DexDebugItemOpcode op, uint32_t arg1,
      uint32_t arg2, uint32_t arg3) {
    return handle_default(op);
  }
  virtual void handle_start_local_extended(DexDebugItemOpcode op, uint32_t arg1,
      uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    return handle_default(op);
  }
  virtual void handle_end_local(DexDebugItemOpcode op, uint32_t arg1) {
    return handle_default(op);
  }
  virtual void handle_restart_local(DexDebugItemOpcode op, uint32_t arg1) {
    return handle_default(op);
  }
  virtual void handle_set_file(DexDebugItemOpcode op, uint32_t arg) {
    return handle_default(op);
  }
  virtual void handle_set_prologue_end(DexDebugItemOpcode op) {
    return handle_default(op);
  }
  virtual void handle_set_epilogue_begin(DexDebugItemOpcode op) {
    return handle_default(op);
  }
  virtual void handle_default(DexDebugItemOpcode op) = 0;
 public:
  virtual ~DexDebugInstructionReader() {}

  void read(const uint8_t*& data) {
    uint32_t u1, u2, u3, u4;
    int32_t s1;
    while (true) {
      DexDebugItemOpcode op = (DexDebugItemOpcode)*data++;
      switch (op) {
      case DBG_END_SEQUENCE:
        return;
      case DBG_ADVANCE_PC:
        u1 = read_uleb128(&data);
        handle_advance_pc(op, u1);
        break;
      case DBG_ADVANCE_LINE:
        s1 = read_sleb128(&data);
        handle_advance_line(op, s1);
        break;
      case DBG_START_LOCAL:
        u1 = read_uleb128(&data);
        u2 = read_uleb128(&data);
        u3 = read_uleb128(&data);
        handle_start_local(op, u1, u2, u3);
        break;
      case DBG_START_LOCAL_EXTENDED:
        u1 = read_uleb128(&data);
        u2 = read_uleb128(&data);
        u3 = read_uleb128(&data);
        u4 = read_uleb128(&data);
        handle_start_local_extended(op, u1, u2, u3, u4);
        break;
      case DBG_END_LOCAL:
        u1 = read_uleb128(&data);
        handle_end_local(op, u1);
        break;
      case DBG_RESTART_LOCAL:
        u1 = read_uleb128(&data);
        handle_restart_local(op, u1);
        break;
      case DBG_SET_PROLOGUE_END:
        handle_set_prologue_end(op);
        break;
      case DBG_SET_EPILOGUE_BEGIN:
        handle_set_epilogue_begin(op);
        break;
      case DBG_SET_FILE:
        u1 = read_uleb128(&data);
        handle_set_file(op, u1);
        break;
      default: // special opcodes
        handle_default(op);
        break;
      };
    }
  }
};

uint32_t count_debug_instructions(const uint8_t*& encdata) {
  struct DexDebugInstructionCounter : public DexDebugInstructionReader {
    int sum;
    void handle_default(DexDebugItemOpcode op) override { sum++; }
  };
  auto counter = DexDebugInstructionCounter();
  counter.read(encdata);
  return counter.sum;
}

void disassemble_debug(ddump_data* rd, uint32_t offset) {
  redump("Disassembling debug opcodes at 0x%x\n", offset);
  auto data = (const uint8_t*)(rd->dexmmap + offset);
  auto line_start = read_uleb128(&data);
  auto parameters_size = read_uleb128(&data);
  redump("line_start: %d, parameters_size: %d\n", line_start, parameters_size);
  for (unsigned int i = 0; i < parameters_size; ++i) {
    read_uleb128(&data);
  }
  struct DexDebugInstructionPrinter : public DexDebugInstructionReader {
    void handle_advance_pc(DexDebugItemOpcode op, uint32_t arg) override {
      redump("DBG_ADVANCE_PC %u\n", arg);
    }
    void handle_advance_line(DexDebugItemOpcode op, int32_t arg) override {
      redump("DBG_ADVANCE_LINE %d\n", arg);
    }
    void handle_start_local(DexDebugItemOpcode op,
                            uint32_t reg,
                            uint32_t name_idx,
                            uint32_t type_idx) override {
      redump("DBG_START_LOCAL %d\n", reg);
    }
    void handle_start_local_extended(DexDebugItemOpcode op,
                                     uint32_t reg,
                                     uint32_t,
                                     uint32_t,
                                     uint32_t) override {
      redump("DBG_START_LOCAL_EXTENDED %d\n", reg);
    }
    void handle_end_local(DexDebugItemOpcode op, uint32_t reg) override {
      redump("DBG_END_LOCAL %d\n", reg);
    }
    void handle_restart_local(DexDebugItemOpcode op, uint32_t reg) override {
      redump("DBG_RESTART_LOCAL %d\n", reg);
    }
    void handle_set_file(DexDebugItemOpcode op, uint32_t arg) override {
      redump("DBG_SET_FILE\n");
    }
    void handle_set_prologue_end(DexDebugItemOpcode op) override {
      redump("DBG_SET_PROLOGUE_END\n");
    }
    void handle_set_epilogue_begin(DexDebugItemOpcode op) override {
      redump("DBG_SET_EPILOGUE_BEGIN\n");
    }
    void handle_default(DexDebugItemOpcode op) override {
      redump("DBG_SPECIAL 0x%02x\n", (uint32_t)op);
    }
  };
  DexDebugInstructionPrinter().read(data);
}

static std::string get_debug_item(const uint8_t** pdebug_item) {
  auto line_start = read_uleb128(pdebug_item);
  auto parameters_size = read_uleb128(pdebug_item);
  for (unsigned int i = 0; i < parameters_size; ++i) {
    read_uleb128(pdebug_item);
  }
  auto num_opcodes = count_debug_instructions(*pdebug_item);
  std::ostringstream ss;
  ss << "line_start: " << line_start << ", "
     << "parameters_size: " << parameters_size << ", "
     << "num_opcodes: " << num_opcodes << "\n";
  return ss.str();
}

// Dump a string_data_item (i.e., an entry in the string data
// section), advancing POS_INOUT over the item.
static const char string_data_header[] = "u16len [contents]";
static void dump_string_data_item(const uint8_t** pos_inout) {
  const uint8_t* pos = *pos_inout;
  uint32_t utf16_code_point_count = read_uleb128(&pos); // Not byte count!
  size_t utf8_length = strlen((char*) pos);
  std::string cleansed_data;
  const char* string_to_print;
  if (raw) { // Output whatever bytes we have
    string_to_print = (char*) pos;
  } else if (escape) { // Escape non-printable characters.
    cleansed_data.reserve(utf8_length);  // Avoid some reallocation.
    for (size_t i = 0; i < utf8_length; i++) {
      if (isprint(pos[i])) {
        cleansed_data.push_back(pos[i]);
      } else {
        char buf[5];
        sprintf(buf, "\\x%02x", pos[i]);
        cleansed_data.append(buf);
      }
    }
    string_to_print = cleansed_data.c_str();
  } else { // Translate to UTF-8; strip control characters
    std::vector<char32_t> code_points;
    const char* enc_pos = (char*) pos;
    uint32_t cp;
    while ((cp = mutf8_next_code_point(enc_pos))) {
      if (cp < ' ' || cp == 255 /* DEL */) {
        cp = '.';
      }
      code_points.push_back(cp);
    }
    ssize_t nr_utf8_bytes = utf32_to_utf8_length(
      &code_points[0], code_points.size());
    if (nr_utf8_bytes < 0 && utf8_length == 0) {
      cleansed_data = "";
    } else if (nr_utf8_bytes < 0) {
      cleansed_data = "{invalid encoding?}";
    } else {
      cleansed_data.resize(nr_utf8_bytes);
      utf32_to_utf8(&code_points[0], code_points.size(), &cleansed_data[0]);
    }
    string_to_print = cleansed_data.c_str();
  }
  redump("%03u [%s]\n", (unsigned) utf16_code_point_count, string_to_print);
  *pos_inout = pos + utf8_length + 1;
}

void dump_stringdata(ddump_data* rd, bool print_headers) {
  if (print_headers) {
    redump("\nRAW STRING DATA\n");
    redump("%s\n", string_data_header);
  }
  dex_map_item* string_data = get_dex_map_item(rd, TYPE_STRING_DATA_ITEM);
  if (string_data == nullptr) {
    redump("!!!! No string data section found\n");
    return;
  }

  // Recall that for dex_map_item, size is really a count.  Each item
  // in the string data section is a ULEB128 length followed by a
  // NUL-terminated modified-UTF-8 encoded string.

  const uint8_t* str_data_ptr =
    (uint8_t*) (rd->dexmmap) + string_data->offset;
  for (uint32_t i = 0; i < string_data->size; ++i) {
    dump_string_data_item(&str_data_ptr);
  }
}

//
// Table dumpers...
//

void dump_strings(ddump_data* rd, bool print_headers) {
  auto offset = rd->dexh->string_ids_off;
  const uint8_t* str_id_ptr = (uint8_t*)(rd->dexmmap) + offset;
  auto size = rd->dexh->string_ids_size;
  auto length = 0;
  uint32_t tmp_str_id_off = 0;
  for (uint32_t i = 0; i < size; ++i) {
    const uint8_t* str_data_ptr = (uint8_t*)(rd->dexmmap) + tmp_str_id_off;
    length += strlen((char*) str_data_ptr);
    tmp_str_id_off += 4;
  }

  if (print_headers) {
    redump("\nSTRING IDS TABLE: %d %d\n", size, length);
    redump("%s\n", string_data_header);
  }
  for (uint32_t i = 0; i < size; ++i) {
    auto str_data_off = *(uint32_t*)str_id_ptr;
    str_id_ptr += 4;
    const uint8_t* str_data_ptr = (uint8_t*)(rd->dexmmap) + str_data_off;
    dump_string_data_item(&str_data_ptr);
  }
}

void dump_types(ddump_data* rd) {
  auto offset = rd->dexh->type_ids_off;
  const uint32_t* type_id_ptr = (uint32_t*)(rd->dexmmap + offset);
  const uint32_t* type_ptr = type_id_ptr;
  auto size = rd->dexh->type_ids_size;
  redump("\nTYPE IDS TABLE: %d\n", size);
  redump("[type_ids_off] type name\n");
  for (uint32_t i = 0; i < size; ++i) {
    auto name_off = *(uint32_t*)type_ptr;
    redump((uint32_t)(type_ptr - type_id_ptr),
           "%s\n",
           dex_string_by_idx(rd, name_off));
    type_ptr++;
  }
}

void dump_protos(ddump_data* rd, bool print_headers) {
  auto size = rd->dexh->proto_ids_size;
  if (print_headers) {
    redump("\nPROTO IDS TABLE: %d\n", size);
    redump("[proto_ids_off] shorty proto\n");
  }
  for (uint32_t i = 0; i < size; i++) {
    redump(i, "%s\n", get_proto(rd, i).c_str());
  }
}

void dump_fields(ddump_data* rd, bool print_headers) {
  auto size = rd->dexh->field_ids_size;
  if (print_headers) {
    redump("\nFIELD IDS TABLE: %d\n", size);
    redump("[field_ids_off] class type name\n");
  }
  for (uint32_t i = 0; i < size; i++) {
    redump(i, "%s\n", get_field(rd, i).c_str());
  }
}

void dump_methods(ddump_data* rd, bool print_headers) {
  uint32_t size = rd->dexh->method_ids_size;
  if (print_headers) {
    redump("\nMETHOD IDS TABLE: %d\n", size);
    redump("[method_ids_off] class name proto_no_shorty\n");
  }
  for (uint32_t i = 0; i < size; i++) {
    redump(i, "%s\n", get_method(rd, i).c_str());
  }
}

void dump_clsdefs(ddump_data* rd, bool print_headers) {
  auto size = rd->dexh->class_defs_size;
  if (print_headers) {
    redump("\nCLASS DEFS TABLE: %d\n", size);
    redump(
        "[class_def_off] flags class 'extends' superclass"
        "['implements' interfaces]\n"
        "\t[file: <filename>] [anno: annotation_off] data: class_data_off "
        "[static values: static_value_off]\n");
  }
  for (uint32_t i = 0; i < size; i++) {
    redump(i, "%s\n", get_class_def(rd, i).c_str());
  }
}

void dump_clsdata(ddump_data* rd, bool print_headers) {
  auto size = rd->dexh->class_defs_size;
  if (print_headers) {
    redump("\nCLASS DATA TABLE: %d\n", size);
    redump(
        "[cls_data_off] class\n"
        "sfields: <count> followed by sfields\n"
        "ifields: <count> followed by ifields\n"
        "dmethods: <count> followed by dmethods\n"
        "vmethods: <count> followed by vmethods\n");
  }
  for (uint32_t i = 0; i < size; i++) {
    const dex_class_def* class_defs =
        (dex_class_def*)(rd->dexmmap + rd->dexh->class_defs_off) + i;
    redump(class_defs->class_data_offset,
           "%s",
           get_class_data_item(rd, i).c_str());
  }
}

static void dump_code_items(ddump_data* rd,
                            dex_code_item* code_items,
                            uint32_t size) {
  for (uint32_t i = 0; i < size; i++) {
    auto offset = reinterpret_cast<char*>(code_items) - rd->dexmmap;
    redump(offset, "%s", get_code_item(&code_items).c_str());
  }
}

static void dump_debug_items(ddump_data* rd,
                             const uint8_t* debug_items,
                             uint32_t size) {
  for (uint32_t i = 0; i < size; i++) {
    auto offset = reinterpret_cast<const char*>(debug_items) - rd->dexmmap;
    redump(offset, "%s", get_debug_item(&debug_items).c_str());
  }
}

void dump_code(ddump_data* rd) {
  unsigned count;
  dex_map_item* maps;
  get_dex_map_items(rd, &count, &maps);
  redump("\nCODE ITEM: %d\n", count);
  redump(
      "[code_item_off] meth_id "
      "registers_size: <count>,"
      "ins_size: <count>,"
      "outs_size: <count>,"
      "tries_size: <count>,"
      "debug_info_off: <addr>,"
      "insns_size: <count>\n");
  for (unsigned i = 0; i < count; i++) {
    if (maps[i].type == TYPE_CODE_ITEM) {
      auto code_items = (dex_code_item*)(rd->dexmmap + maps[i].offset);
      dump_code_items(rd, code_items, maps[i].size);
      return;
    }
  }
}

static void dump_annotation_set_item(ddump_data* rd, uint32_t* aset) {
  uint32_t count = *aset++;
  if (count == 0) {
    redump("Empty Aset\n");
  }
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t* aitem = (uint8_t*)(rd->dexmmap + aset[i]);
    redump(format_annotation_item(rd, &aitem).c_str());
  }
}

static void dump_class_annotations(ddump_data* rd, dex_class_def* df) {
  char* cname = dex_string_by_type_idx(rd, df->typeidx);
  if (df->annotations_off) {
    uint32_t* diritem = (uint32_t*)(rd->dexmmap + df->annotations_off);
    uint32_t aclass, afields, amethods, aparams;
    aclass = *diritem++;
    afields = *diritem++;
    amethods = *diritem++;
    aparams = *diritem++;
    redump(df->typeidx, "Class '%s':\n", cname);
    if (aclass) {
      redump("    Class Annotations:\n");
      dump_annotation_set_item(rd, (uint32_t*)(rd->dexmmap + aclass));
    }
    while (afields--) {
      uint32_t fidx = *diritem++;
      uint32_t aoff = *diritem++;
      const char* ftype =
          dex_string_by_type_idx(rd, rd->dex_field_ids[fidx].typeidx);
      const char* fname =
          dex_string_by_idx(rd, rd->dex_field_ids[fidx].nameidx);
      redump("    Field '%s', Type '%s' Annotations:\n", ftype, fname);
      dump_annotation_set_item(rd, (uint32_t*)(rd->dexmmap + aoff));
    }
    while (amethods--) {
      uint32_t midx = *diritem++;
      uint32_t aoff = *diritem++;
      const char* mtype =
          dex_string_by_type_idx(rd, rd->dex_method_ids[midx].classidx);
      const char* mname =
          dex_string_by_idx(rd, rd->dex_method_ids[midx].nameidx);
      redump("    Method '%s', Type '%s' Annotations:\n", mtype, mname);
      dump_annotation_set_item(rd, (uint32_t*)(rd->dexmmap + aoff));
    }
    while (aparams--) {
      uint32_t midx = *diritem++;
      uint32_t asrefoff = *diritem++;
      uint32_t* asref = (uint32_t*)(rd->dexmmap + asrefoff);
      uint32_t asrefsize = *asref++;
      const char* mtype =
          dex_string_by_type_idx(rd, rd->dex_method_ids[midx].classidx);
      const char* mname =
          dex_string_by_idx(rd, rd->dex_method_ids[midx].nameidx);
      redump(
          "    Method '%s', Type '%s' Parameter Annotations:\n", mtype, mname);
      int param = 0;
      while (asrefsize--) {
        uint32_t aoff = *asref++;
        redump("%d: ", param++);
        dump_annotation_set_item(rd, (uint32_t*)(rd->dexmmap + aoff));
      }
    }
  }
}

void dump_anno(ddump_data* rd) {
  for (uint32_t i = 0; i < rd->dexh->class_defs_size; i++) {
    dump_class_annotations(rd, &rd->dex_class_defs[i]);
  }
}

void dump_debug(ddump_data* rd) {
  unsigned count;
  dex_map_item* maps;
  get_dex_map_items(rd, &count, &maps);
  for (unsigned i = 0; i < count; i++) {
    if (maps[i].type == TYPE_DEBUG_INFO_ITEM) {
      auto debug_items = (uint8_t*)(rd->dexmmap + maps[i].offset);
      dump_debug_items(rd, debug_items, maps[i].size);
      return;
    }
  }
}

void dump_enarr(ddump_data* rd) {
  unsigned count;
  dex_map_item* maps;
  get_dex_map_items(rd, &count, &maps);
  for (unsigned i = 0; i < count; i++) {
    if (maps[i].type == TYPE_ENCODED_ARRAY_ITEM) {
      auto ptr = (const uint8_t*)(rd->dexmmap + maps[i].offset);
      for (unsigned j = 0; j < maps[i].size; j++) {
        redump((uint32_t)(ptr - (const uint8_t*)rd->dexmmap), ": ");
        uint32_t earray_size = read_uleb128(&ptr);
        for (uint32_t k = 0; k < earray_size; k++) {
          redump("%s", format_encoded_value(rd, &ptr).c_str());
        }
        redump("\n");
      }
      return;
    }
  }
}
