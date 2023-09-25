/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"

namespace klass {

struct Serdes {
  std::vector<DexType*> serdes;

  Serdes(DexType* deser,
         DexType* flatbuf_deser,
         DexType* ser,
         DexType* flatbuf_ser)
      : serdes{deser, flatbuf_deser, ser, flatbuf_ser} {}

  std::vector<DexType*> get_all_serdes() { return serdes; }

  DexType* get_deser() { return serdes[0]; }
  DexType* get_flatbuf_deser() { return serdes[1]; }
  DexType* get_ser() { return serdes[2]; }
  DexType* get_flatbuf_ser() { return serdes[3]; }
};

/**
 * Return possible deserializer and serializer classes of the given class
 * 'class$Deserializer;', 'class_Deserializer;', 'class$Serializer;',
 * 'class_Serializer;'
 */
Serdes get_serdes(const DexClass* cls);

/**
 * Looks for a <clinit> method for the given class, creates a new one if it
 * does not exist
 */
DexMethod* get_or_create_clinit(DexClass* cls, bool need_editable_cfg = false);

/**
 * Return true if the parent chain leads to known classes.
 * False if one of the parent is in a scope unknown to redex.
 */
bool has_hierarchy_in_scope(DexClass* cls);

}; // namespace klass
