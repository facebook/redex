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
#include "Trace.h"

bool load_proguard_config_file(const char *location, std::vector<KeepRule>* rules,
                               std::vector<std::string>* library_jars) {
  TRACE(MAIN, 1, "Loading ProGuard configuration from %s\n", location);
  parse_proguard_file(location, rules, library_jars);
  if (rules->empty()) {
    fprintf(stderr, "couldn't parse Proguard rules\n");
    return false;
  } else {
    TRACE(PGR, 1, "Loaded %d ProGuard rules\n", rules->size());
    return true;
  }
}
