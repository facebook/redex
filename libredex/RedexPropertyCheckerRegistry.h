/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

namespace redex_properties {

class PropertyChecker;

/**
 * Global registry of property checkers.  Construction of a checker
 * automatically registers it here.  Checkers should be constructed statically
 * before main.
 */
struct PropertyCheckerRegistry {
  /**
   * Get the global registry object.
   */
  static PropertyCheckerRegistry& get();

  /**
   * Register a pass.
   */
  void register_checker(PropertyChecker* checker);

  /**
   * Get the checkers.
   */
  const std::vector<PropertyChecker*>& get_checkers() const;

 private:
  /**
   * Singleton.  Private/deleted constructors.
   */
  PropertyCheckerRegistry() {}
  PropertyCheckerRegistry(const PropertyCheckerRegistry&) = delete;
  PropertyCheckerRegistry& operator=(const PropertyCheckerRegistry&) = delete;

  std::vector<PropertyChecker*> m_registered_checkers;
};

} // namespace redex_properties
