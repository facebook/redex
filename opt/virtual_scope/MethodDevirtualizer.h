/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Pass.h"

struct DevirtualizerConfigs {
  bool vmethods_not_using_this = true;
  bool vmethods_using_this = false;
  bool dmethods_not_using_this = true;
  bool dmethods_using_this = false;

  DevirtualizerConfigs(bool vmethods_not_using_this = true,
                       bool vmethods_using_this = false,
                       bool dmethods_not_using_this = true,
                       bool dmethods_using_this = false)
      : vmethods_not_using_this(vmethods_not_using_this),
        vmethods_using_this(vmethods_using_this),
        dmethods_not_using_this(dmethods_not_using_this),
        dmethods_using_this(dmethods_using_this) {}
};

struct DevirtualizerMetrics {
  uint32_t num_methods_not_using_this;
  uint32_t num_methods_using_this;
  uint32_t num_virtual_calls;
  uint32_t num_direct_calls;
  uint32_t num_super_calls;
};

class MethodDevirtualizer {
 public:
  explicit MethodDevirtualizer(DevirtualizerConfigs& config)
      : m_config(config) {}
  MethodDevirtualizer(bool vmethods_not_using_this,
                      bool vmethods_using_this,
                      bool dmethods_not_using_this,
                      bool dmethods_using_this) {
    m_config = {vmethods_not_using_this,
                vmethods_using_this,
                dmethods_not_using_this,
                dmethods_using_this};
  }

  DevirtualizerMetrics devirtualize_methods(
      DexStoresVector& stores, const std::vector<DexClass*>& target_classes);

  DevirtualizerMetrics devirtualize_methods(DexStoresVector& stores);

  // Assuming vmethods.
  DevirtualizerMetrics devirtualize_vmethods(
      DexStoresVector& stores, const std::vector<DexMethod*>& methods);

 private:
  DevirtualizerConfigs m_config;
  DevirtualizerMetrics m_metrics = {0, 0, 0, 0, 0};

  void reset_metrics() { m_metrics = {0, 0, 0, 0, 0}; }

  void staticize_methods_using_this(
      const std::vector<DexClass*>& scope,
      const std::unordered_set<DexMethod*>& methods);

  void staticize_methods_not_using_this(
      const std::vector<DexClass*>& scope,
      const std::unordered_set<DexMethod*>& methods);
};
