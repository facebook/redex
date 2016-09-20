/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <vector>

class Pass;

/**
 * Global registry of passes.  Construction of a pass automatically registers
 * it here.  Passes should be constructed statically before main.
 */
struct PassRegistry {
  /**
   * Get the global registry object.
   */
  static PassRegistry& get();

  /**
   * Register a pass.
   */
  void register_pass(Pass* pass);

  /**
   * Get the passes.
   */
  const std::vector<Pass*>& get_passes() const;

 private:
  /**
   * Singleton.  Private/deleted constructors.
   */
  PassRegistry() {}
  PassRegistry(const PassRegistry&) = delete;

  std::vector<Pass*> m_registered_passes;
};
