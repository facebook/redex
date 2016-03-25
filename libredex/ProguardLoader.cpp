/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <vector>

#include "ProguardLoader.h"

bool load_proguard_config_file(const char *location, std::vector<KeepRule>* rules) {
  parse_proguard_file(location, rules);
  if (rules->empty()) {
    fprintf(stderr, "couldn't parse Proguard rules\n");
    return false;
  } else {
    return true;
  }
}
