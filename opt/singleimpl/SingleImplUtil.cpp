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
    auto element_type = get_array_element_type(type);
    redex_assert(element_type);
    lookup_type = element_type;
  }
  const auto& intf_data = single_impls.find(lookup_type);
  if (intf_data != single_impls.end()) {
    auto concrete = intf_data->second.cls;
    return make_array_type(concrete, array_level);
  }
  return nullptr;
}
