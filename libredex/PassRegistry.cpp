/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "PassRegistry.h"

PassRegistry& PassRegistry::get() {
  static PassRegistry registry;
  return registry;
}

void PassRegistry::register_pass(Pass* pass) {
  m_registered_passes.push_back(pass);
}

const std::vector<Pass*>& PassRegistry::get_passes() const {
  return m_registered_passes;
}
