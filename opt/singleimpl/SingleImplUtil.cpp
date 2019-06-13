/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SingleImplUtil.h"

#include "DexUtil.h"

DexType* get_concrete_type(SingleImpls& single_impls, DexType* type) {
  DexType* lookup_type = type;
  uint32_t array_level = get_array_level(type);
  if (array_level > 0) {
    auto array_type = get_array_type(type);
    redex_assert(array_type);
    lookup_type = array_type;
  }
  const auto& intf_data = single_impls.find(lookup_type);
  if (intf_data != single_impls.end()) {
    auto concrete = intf_data->second.cls;
    if (array_level == 0) {
      return concrete;
    }
    const auto base_name = concrete->get_name()->c_str();
    const uint32_t size = (uint32_t)(array_level + strlen(base_name));
    auto array_name = std::make_unique<char[]>(size);
    char* p = array_name.get();
    while (array_level--)
      *p++ = '[';
    strcpy(p, concrete->get_name()->c_str());
    auto array_type = DexType::get_type(array_name.get(), size);
    return array_type;
  }
  return nullptr;
}
