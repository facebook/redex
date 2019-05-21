/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include "ConcurrentContainers.h"

namespace optimize_enums {
struct Config {
  /**
   * We create a helper class `EnumUtils` in primary dex with all the boxed
   * integer fields for representing enum values. The maximum number of the
   * fields is equal to largest number of values of candidate enum classes. To
   * limit the size of the class, exclude the enum classes that contain more
   * than max_enum_size values before the transformation.
   */
  uint32_t max_enum_size{100};
  ConcurrentSet<DexType*> candidate_enums;
};
} // namespace optimize_enums
