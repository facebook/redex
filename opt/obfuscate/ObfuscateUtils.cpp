/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ObfuscateUtils.h"
#include "Trace.h"
#include <algorithm>
#include "Walkers.h"
#include "DexClass.h"

DexFieldManager new_dex_field_manager() {
  return DexFieldManager(
      [](DexField*& f) -> FieldNameWrapper* { return new FieldNameWrapper(f); },
      [](DexFieldRef* f) -> DexType* { return f->get_type(); },
      [](const std::string& new_name) -> DexFieldSpec {
        DexFieldSpec spec;
        spec.name = DexString::make_string(new_name);
        return spec;
      });
}

DexMethodManager new_dex_method_manager() {
  return DexMethodManager(
      [](DexMethod*& f) -> MethodNameWrapper* {
        return new MethodNameWrapper(f);
      },
      [](DexMethodRef* m) -> DexProto* { return m->get_proto(); },
      [](const std::string& new_name) -> DexMethodSpec {
        DexMethodSpec spec;
        spec.name = DexString::make_string(new_name);
        return spec;
      });
}
