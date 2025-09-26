/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IRMetaIO.h"

#include <fstream>
#include <iostream>

#include "DexEncoding.h"
#include "Show.h"
#include "StringBuilder.h"
#include "Walkers.h"

namespace {
constexpr const char* IRMETA_FILE_NAME = "/irmeta.bin";

constexpr const char* IRMETA_MAGIC_NUMBER = "rdx.\n\x14\x12\x00";

PACKED(struct ir_meta_header_t {
  char magic[8];
  uint32_t checksum; // reserved
  uint32_t file_size;
  uint32_t classes_size;
  uint32_t rstate_size; // size of IRMetaIO::bit_rstate_t.
});

void serialize_str(const std::string_view str, std::ofstream& ostrm) {
  char data[5];
  write_uleb128((uint8_t*)data, str.size());
  ostrm.write(data, uleb128_encoding_size(str.size()));
  ostrm.write(str.data(), str.size());
  ostrm.put('\0');
}

DexField* find_field(const DexClass* cls, const std::string& name) {
  auto result =
      std::find_if(cls->get_sfields().begin(),
                   cls->get_sfields().end(),
                   [&](const DexField* f) { return f->str() == name; });
  if (result != cls->get_sfields().end()) {
    return *result;
  }
  auto result2 =
      std::find_if(cls->get_ifields().begin(),
                   cls->get_ifields().end(),
                   [&](const DexField* f) { return f->str() == name; });
  redex_assert(result2 != cls->get_ifields().end());
  return *result2;
}

DexMethod* find_method(const DexClass* cls, const std::string& name_and_proto) {
  int colon = name_and_proto.find(':');
  const DexString* method_name =
      DexString::make_string(name_and_proto.substr(0, colon));
  std::string proto =
      name_and_proto.substr(colon + 1, name_and_proto.size() - colon - 1);
  auto result = std::find_if(
      cls->get_dmethods().begin(), cls->get_dmethods().end(),
      [&](const DexMethod* m) {
        return m->get_name() == method_name && show(m->get_proto()) == proto;
      });
  if (result != cls->get_dmethods().end()) {
    return *result;
  }
  auto result2 = std::find_if(
      cls->get_vmethods().begin(), cls->get_vmethods().end(),
      [&](const DexMethod* m) {
        return m->get_name() == method_name && show(m->get_proto()) == proto;
      });
  redex_assert(result2 != cls->get_vmethods().end());
  return *result2;
}

/**
 * Serialize deobfuscated_name and rstate of class, method or field.
 */
template <typename T>
void serialize_name_and_rstate(const T* obj, std::ofstream& ostrm) {
  if (show(obj) != obj->get_deobfuscated_name_or_empty()) {
    serialize_str(obj->get_deobfuscated_name_or_empty(), ostrm);
  } else {
    serialize_str("", ostrm);
  }
  ir_meta_io::IRMetaIO::serialize_rstate(obj->rstate, ostrm);
}

template <typename T>
void deserialize_name_and_rstate(const char** _ptr, T* obj) {
  auto size = read_uleb128((const uint8_t**)_ptr);
  if (size) {
    // In a corrupted input *_ptr may not be null terminated.
    obj->set_deobfuscated_name(std::string(*_ptr, size));
  } else {
    obj->set_deobfuscated_name(show(obj));
  }
  (*_ptr) += size + 1;
  ir_meta_io::IRMetaIO::deserialize_rstate(_ptr, obj->rstate);
}

enum BlockType : char { ClassBlock, FieldBlock, MethodBlock, EndOfBlock };

/**
 * Serialize meta data of classes into binary format.
 * A class's meta looks like this:
 *  class_name
 *  deobfuscated_name
 *  ReferencedState
 *    field1_name
 *    deobfuscated_name
 *    ReferencedState
 *    field2_name
 *    ...
 *    method1_name
 *    deobfuscated_name
 *    ReferencedState
 *    method2_name
 *    ...
 *  ...
 */
void serialize_class_data(const Scope& classes, std::ofstream& ostrm) {
  walk::classes(classes, [&](const DexClass* cls) {
    // Fields
    std::vector<const DexField*> fields;
    for (const DexField* field : cls->get_sfields()) {
      if (!ir_meta_io::IRMetaIO::is_default_meta(field)) {
        fields.push_back(field);
      }
    }
    for (const DexField* field : cls->get_ifields()) {
      if (!ir_meta_io::IRMetaIO::is_default_meta(field)) {
        fields.push_back(field);
      }
    }
    // Methods
    std::vector<const DexMethod*> methods;
    for (const DexMethod* method : cls->get_dmethods()) {
      if (!ir_meta_io::IRMetaIO::is_default_meta(method)) {
        methods.push_back(method);
      }
    }
    for (const DexMethod* method : cls->get_vmethods()) {
      if (!ir_meta_io::IRMetaIO::is_default_meta(method)) {
        methods.push_back(method);
      }
    }

    // Classes
    if (!fields.empty() || !methods.empty() ||
        !ir_meta_io::IRMetaIO::is_default_meta(cls)) {
      ostrm.put(BlockType::ClassBlock);
      serialize_str(cls->c_str(), ostrm);
      serialize_name_and_rstate(cls, ostrm);
    }

    for (const DexField* field : fields) {
      ostrm.put(BlockType::FieldBlock);
      serialize_str(field->c_str(), ostrm);
      serialize_name_and_rstate(field, ostrm);
    }

    for (const DexMethod* method : methods) {
      ostrm.put(BlockType::MethodBlock);
      string_builders::StaticStringBuilder<3> b;
      b << method->c_str() << ":" << show(method->get_proto());
      serialize_str(b.str(), ostrm);
      serialize_name_and_rstate(method, ostrm);
    }
  });
}

void deserialize_class_data(std::ifstream& istrm, uint32_t data_size) {
  auto data = std::make_unique<char[]>(data_size);
  istrm.read((char*)data.get(), data_size);
  char* ptr = data.get();
  DexClass* cls = nullptr;
  while (ptr - data.get() < data_size) {
    BlockType btype = (BlockType)*ptr++;
    always_assert(btype >= 0 && btype < BlockType::EndOfBlock);
    int strsize = read_uleb128((const uint8_t**)&ptr);
    switch (btype) {
    case BlockType::ClassBlock: {
      // Create a std::string for null termination
      DexType* type = DexType::get_type(std::string_view(ptr, strsize));
      cls = type_class(type);
      always_assert(cls != nullptr);
      ptr += strsize + 1;
      deserialize_name_and_rstate((const char**)&ptr, cls);
      break;
    }
    case BlockType::FieldBlock: {
      DexField* field = find_field(cls, std::string(ptr, strsize));
      ptr += strsize + 1;
      deserialize_name_and_rstate((const char**)&ptr, field);
      break;
    }
    case BlockType::MethodBlock: {
      DexMethod* method = find_method(cls, std::string(ptr, strsize));
      ptr += strsize + 1;
      deserialize_name_and_rstate((const char**)&ptr, method);
      break;
    }
    default: {
      not_reached();
    }
    }
  }
}
} // namespace

namespace ir_meta_io {

void dump(const Scope& classes, const std::string& output_dir) {
  std::string output_file = output_dir + IRMETA_FILE_NAME;
  std::ofstream ostrm(output_file, std::ios::binary | std::ios::trunc);

  ir_meta_header_t meta_header;
  memcpy(meta_header.magic, IRMETA_MAGIC_NUMBER, 8);
  meta_header.checksum = 0;
  meta_header.file_size = 0;
  meta_header.classes_size = 0;
  meta_header.rstate_size = sizeof(IRMetaIO::bit_rstate_t);
  ostrm.write((char*)&meta_header, sizeof(meta_header));

  serialize_class_data(classes, ostrm);
  meta_header.classes_size = (uint32_t)ostrm.tellp() - sizeof(meta_header);

  // TODO(fengliu): Serialize pass related data

  meta_header.file_size = ostrm.tellp();
  ostrm.seekp(0);
  ostrm.write((char*)&meta_header, sizeof(meta_header));
}

bool load(const std::string& input_dir) {
  std::string input_file = input_dir + IRMETA_FILE_NAME;
  std::ifstream istrm(input_file, std::ios::binary | std::ios::in);
  if (!istrm.is_open()) {
    std::cerr << "Can not open " << input_file << std::endl;
    return false;
  }

  ir_meta_header_t meta_header;
  istrm.read((char*)&meta_header, sizeof(meta_header));
  if (strcmp(meta_header.magic, IRMETA_MAGIC_NUMBER) != 0) {
    std::cerr << "May be not valid meta file\n";
    return false;
  }
  if (meta_header.rstate_size != sizeof(IRMetaIO::bit_rstate_t)) {
    std::cerr << "Could not load the outdated IR meta data\n";
    return false;
  }
  // file size

  deserialize_class_data(istrm, meta_header.classes_size);

  return true;
}

void IRMetaIO::serialize_rstate(const ReferencedState& rstate,
                                std::ofstream& ostrm) {
  bit_rstate_t bit_rstate;
  bit_rstate.inner_struct = rstate.inner_struct;
  ostrm.write((char*)&bit_rstate, sizeof(bit_rstate));
}

void IRMetaIO::deserialize_rstate(const char** _ptr, ReferencedState& rstate) {
  bit_rstate_t* bit_rstate = (bit_rstate_t*)(*_ptr);
  rstate.inner_struct = bit_rstate->inner_struct;
  (*_ptr) += sizeof(bit_rstate_t);
}

} // namespace ir_meta_io
