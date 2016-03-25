/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Formatters.h"
#include "PrintUtil.h"
#include <RedexDump.h>
#include <sstream>
#include <string.h>
#include <string>

/**
 * Return a proto string in the form
 * [shorty] (argTypes)returnType
 */
static std::string get_proto(ddump_data* rd,
                             uint32_t idx,
                             bool with_shorty = true) {
  std::stringstream ss;
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
  std::stringstream ss;
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
  std::stringstream ss;
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
  std::stringstream ss;
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
  std::stringstream ss;
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
  std::stringstream ss;
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
  std::stringstream ss;
  ss << "registers_size: " << code_item->registers_size << ", "
     << "ins_size: " << code_item->ins_size << ", "
     << "outs_size: " << code_item->outs_size << ", "
     << "tries_size: " << code_item->tries_size << ", "
     << "debug_info_off: " << code_item->debug_info_off << ", "
     << "insns_size: " << code_item->insns_size << "\n";
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

// Dump a string_data_item (i.e., an entry in the string data
// section), advancing POS_INOUT over the item.  BUG: we dump the
// MUTF8 string as if it were standard UTF8.  Shame on us.
static const char string_data_header[] = "u16len [contents]";
static void dump_string_data_item(const uint8_t** pos_inout) {
  const uint8_t* pos = *pos_inout;
  uint32_t utf16_code_point_count = read_uleb128(&pos); // Not byte count!
  size_t utf8_length = strlen((char*) pos);
  redump("%03u [%s]\n", (unsigned) utf16_code_point_count, pos);
  *pos_inout = pos + utf8_length + 1;
}

void dump_stringdata(ddump_data* rd) {
  redump("\nRAW STRING DATA\n");
  redump("%s\n", string_data_header);
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

void dump_strings(ddump_data* rd) {
  auto offset = rd->dexh->string_ids_off;
  const uint8_t* str_id_ptr = (uint8_t*)(rd->dexmmap) + offset;
  auto size = rd->dexh->string_ids_size;
  redump("\nSTRING IDS TABLE: %d\n", size);
  redump("%s\n", string_data_header);
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

void dump_protos(ddump_data* rd) {
  auto size = rd->dexh->proto_ids_size;
  redump("\nPROTO IDS TABLE: %d\n", size);
  redump("[proto_ids_off] shorty proto\n");
  for (uint32_t i = 0; i < size; i++) {
    redump(i, "%s\n", get_proto(rd, i).c_str());
  }
}

void dump_fields(ddump_data* rd) {
  auto size = rd->dexh->field_ids_size;
  redump("\nFIELD IDS TABLE: %d\n", size);
  redump("[field_ids_off] class type name\n");
  for (uint32_t i = 0; i < size; i++) {
    redump(i, "%s\n", get_field(rd, i).c_str());
  }
}

void dump_methods(ddump_data* rd) {
  uint32_t size = rd->dexh->method_ids_size;
  redump("\nMETHOD IDS TABLE: %d\n", size);
  redump("[method_ids_off] class name proto_no_shorty\n");
  for (uint32_t i = 0; i < size; i++) {
    redump(i, "%s\n", get_method(rd, i).c_str());
  }
}

void dump_clsdefs(ddump_data* rd) {
  auto size = rd->dexh->class_defs_size;
  redump("\nCLASS DEFS TABLE: %d\n", size);
  redump(
      "[class_def_off] flags class 'extends' superclass"
      "['implements' interfaces]\n"
      "\t[file: <filename>] [anno: annotation_off] data: class_data_off "
      "[static values: static_value_off]\n");
  for (uint32_t i = 0; i < size; i++) {
    redump(i, "%s\n", get_class_def(rd, i).c_str());
  }
}

void dump_clsdata(ddump_data* rd) {
  auto size = rd->dexh->class_defs_size;
  redump("\nCLASS DATA TABLE: %d\n", size);
  redump(
      "[cls_data_off] class\n"
      "sfields: <count> followed by sfields\n"
      "ifields: <count> followed by ifields\n"
      "dmethods: <count> followed by dmethods\n"
      "vmethods: <count> followed by vmethods\n");
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
      "debug_info_off: <count>,"
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

void dump_enarr(ddump_data* rd) {
  unsigned count;
  dex_map_item* maps;
  get_dex_map_items(rd, &count, &maps);
  for (unsigned i = 0; i < count; i++) {
    if (maps[i].type == TYPE_ENCODED_ARRAY_ITEM) {
      auto ptr = (const uint8_t*)(rd->dexmmap + maps[i].offset);
      for (unsigned j = 0; j < maps[i].size; j++) {
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
