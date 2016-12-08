/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ObfuscateUtils.h"
#include "Trace.h"
#include <algorithm>
#include "Walkers.h"
#include "DexClass.h"

DexFieldManager new_dex_field_manager() {
  return DexFieldManager(
      [](DexField*& f) -> FieldNameWrapper* { return new FieldNameWrapper(f); },
      [](DexField*& f) -> DexType* { return f->get_type(); },
      [](const std::string& new_name) -> DexFieldRef {
        DexFieldRef ref;
        ref.name = DexString::make_string(new_name);
        return ref;
      });
}

DexMethodManager new_dex_method_manager() {
  return DexMethodManager(
      [](DexMethod*& f) -> MethodNameWrapper* { return new MethodNameWrapper(f); },
      [](DexMethod*& m) -> DexProto* { return m->get_proto(); },
      [](const std::string& new_name) -> DexMethodRef {
        DexMethodRef ref;
        ref.name = DexString::make_string(new_name);
        return ref;
      });
}
