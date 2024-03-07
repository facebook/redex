/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "RedexProperties.h"

#include "DexStore.h"

struct ConfigFiles;
class PassManager;

namespace redex_properties {

class PropertyChecker {
  const Property m_property;

 public:
  explicit PropertyChecker(Property property);
  virtual ~PropertyChecker();
  const Property& get_property() const { return m_property; }
  virtual void run_checker(DexStoresVector& stores,
                           ConfigFiles& conf,
                           PassManager& mgr,
                           bool established) = 0;
};

} // namespace redex_properties
