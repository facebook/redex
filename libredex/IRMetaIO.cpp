/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IRMetaIO.h"

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
});

PACKED(struct bit_rstate_t {
  uint8_t m_bytype : 1;
  uint8_t m_bystring : 1;
  uint8_t m_byresources : 1;
  uint8_t m_mix_mode : 1;
  uint8_t m_keep : 1;
  uint8_t m_assumenosideeffects : 1;
  uint8_t m_blanket_keepnames : 1;
  uint8_t m_whyareyoukeeping : 1;
  uint8_t m_set_allowshrinking : 1;
  uint8_t m_unset_allowshrinking : 1;
  uint8_t m_set_allowobfuscation : 1;
  uint8_t m_unset_allowobfuscation : 1;
  uint8_t m_keep_name : 1;
  uint16_t m_keep_count;
});

void serialize_str(const std::string& str, std::ofstream& ostrm) {
  char data[5];
  write_uleb128((uint8_t*)data, str.length());
  ostrm.write(data, uleb128_encoding_size(str.length()));
  ostrm.write(str.c_str(), str.length());
  ostrm.write("\0", 1);
}

/**
 * Serialize deobfuscated_name and rstate of class, method or field.
 */
template <typename T>
void serialize_name_and_rstate(const T* obj, std::ofstream& ostrm) {
  if (show(obj) != obj->get_deobfuscated_name()) {
    serialize_str(obj->get_deobfuscated_name(), ostrm);
  } else {
    serialize_str("", ostrm);
  }
  ir_meta_io::IRMetaIO::serialize_rstate(obj->rstate, ostrm);
}

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
      ostrm.put('\0');
      serialize_str(cls->c_str(), ostrm);
      serialize_name_and_rstate(cls, ostrm);
    }

    for (const DexField* field : fields) {
      ostrm.put('\1');
      serialize_str(field->c_str(), ostrm);
      serialize_name_and_rstate(field, ostrm);
    }

    for (const DexMethod* method : methods) {
      ostrm.put('\2');
      string_builders::StaticStringBuilder<3> b;
      b << method->c_str() << ":" << show(method->get_proto());
      serialize_str(b.str(), ostrm);
      serialize_name_and_rstate(method, ostrm);
    }
  });
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
  ostrm.write((char*)&meta_header, sizeof(meta_header));

  serialize_class_data(classes, ostrm);
  meta_header.classes_size = (uint32_t)ostrm.tellp() - sizeof(meta_header);

  // TODO(fengliu): Serialize pass related data

  meta_header.file_size = ostrm.tellp();
  ostrm.seekp(0);
  ostrm.write((char*)&meta_header, sizeof(meta_header));
}

bool load(const std::string& input_dir, RedexContext* rdx_context) {
  // TODO(fengliu)
  return false;
}

void IRMetaIO::serialize_rstate(const ReferencedState& rstate,
                                std::ofstream& ostrm) {
  bit_rstate_t bit_rstate;
  bit_rstate.m_bytype = rstate.m_bytype;
  bit_rstate.m_bystring = rstate.m_bystring;
  bit_rstate.m_byresources = rstate.m_byresources;
  bit_rstate.m_mix_mode = rstate.m_mix_mode;
  bit_rstate.m_keep = rstate.m_keep;
  bit_rstate.m_assumenosideeffects = rstate.m_assumenosideeffects;
  bit_rstate.m_blanket_keepnames = rstate.m_blanket_keepnames;
  bit_rstate.m_whyareyoukeeping = rstate.m_whyareyoukeeping;

  bit_rstate.m_set_allowshrinking = rstate.m_set_allowshrinking;
  bit_rstate.m_unset_allowshrinking = rstate.m_unset_allowshrinking;
  bit_rstate.m_set_allowobfuscation = rstate.m_set_allowobfuscation;
  bit_rstate.m_unset_allowobfuscation = rstate.m_unset_allowobfuscation;
  bit_rstate.m_keep_name = rstate.m_keep_name;
  bit_rstate.m_keep_count = rstate.m_keep_count.load();
  ostrm.write((char*)&bit_rstate, sizeof(bit_rstate));
}

} // namespace ir_meta_io
