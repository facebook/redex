/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "Creators.h"
#include "DexClass.h"
#include "RedexContext.h"

DexField* make_field_ref(DexType* cls, const char* name, DexType* type) {
  return DexField::make_field(cls, DexString::make_string(name), type);
}

DexField* make_field_def(DexType* cls,
                         const char* name,
                         DexType* type,
                         DexAccessFlags access = ACC_PUBLIC,
                         bool external = false) {
  auto field = DexField::make_field(cls, DexString::make_string(name), type);
  if (external) {
    field->set_access(access);
    field->set_external();
  } else {
    field->make_concrete(access);
  }
  return field;
}

DexClass* create_class(DexType* type,
                       DexType* super,
                       std::vector<DexField*> fields,
                       DexAccessFlags access = ACC_PUBLIC,
                       bool external = false) {
  ClassCreator creator(type);
  creator.set_access(access);
  if (external) creator.set_external();
  if (super != nullptr) {
    creator.set_super(super);
  }
  for (const auto& field : fields) {
    creator.add_field(field);
  }
  return creator.create();
}

TEST(RenameMembers, rename) {
  g_redex = new RedexContext();
  auto obj_t = DexType::make_type("Ljava/lang/Object;");
  auto int_t = DexType::make_type("I");
  auto a = DexType::make_type("A");
  auto field = make_field_def(a, "wombat", int_t, ACC_PUBLIC, true);
  auto cls_A = create_class(a, obj_t, {field}, ACC_PUBLIC, true);
  std::string name_before = field->get_name()->c_str();
  ASSERT_EQ("wombat", name_before);
  DexFieldRef ref;
  ref.name = DexString::make_string("numbat");
  field->change(ref);
  std::string name_after = field->get_name()->c_str();
  ASSERT_EQ("numbat", name_after);
}
