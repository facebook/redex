/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
