/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
