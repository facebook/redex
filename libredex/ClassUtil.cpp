/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClassUtil.h"
#include "DexAsm.h"
#include "IRCode.h"
#include "Show.h"
#include "TypeUtil.h"

namespace klass {

Serdes get_serdes(const DexClass* cls) {
  std::string name = cls->get_name()->str_copy();
  name.pop_back();
  std::string flatbuf_name = name;
  std::replace(flatbuf_name.begin(), flatbuf_name.end(), '$', '_');

  std::string desername = name + "$Deserializer;";
  DexType* deser = DexType::get_type(desername.c_str());

  std::string flatbuf_desername = flatbuf_name + "Deserializer;";
  DexType* flatbuf_deser = DexType::get_type(flatbuf_desername.c_str());

  std::string sername = name + "$Serializer;";
  DexType* ser = DexType::get_type(sername);

  std::string flatbuf_sername = flatbuf_name + "Serializer;";
  DexType* flatbuf_ser = DexType::get_type(flatbuf_sername);

  return Serdes(deser, flatbuf_deser, ser, flatbuf_ser);
}

DexMethod* get_or_create_clinit(DexClass* cls) {
  using namespace dex_asm;

  DexMethod* clinit = cls->get_clinit();

  if (clinit) {
    return clinit;
  }

  auto clinit_name = DexString::make_string("<clinit>");
  auto clinit_proto =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));

  // clinit does not exist, create one
  clinit =
      DexMethod::make_method(cls->get_type(), clinit_name, clinit_proto)
          ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_CONSTRUCTOR, false);

  clinit->set_deobfuscated_name(show_deobfuscated(clinit));

  auto ir_code = std::make_unique<IRCode>(clinit, 1);
  ir_code->push_back(dasm(OPCODE_RETURN_VOID));
  clinit->set_code(std::move(ir_code));
  cls->add_method(clinit);
  return clinit;
}

bool has_hierarchy_in_scope(DexClass* cls) {
  DexType* super = nullptr;
  const DexClass* super_cls = cls;
  while (super_cls) {
    super = super_cls->get_super_class();
    super_cls = type_class_internal(super);
  }
  return super == type::java_lang_Object();
}

}; // namespace klass
