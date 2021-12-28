/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodDevirtualizationPass.h"
#include "ConfigFiles.h"
#include "DexUtil.h"
#include "MethodDevirtualizer.h"
#include "PassManager.h"

void MethodDevirtualizationPass::run_pass(DexStoresVector& stores,
                                          ConfigFiles& config,
                                          PassManager& manager) {
  MethodDevirtualizer devirt(
      m_staticize_vmethods_not_using_this, m_staticize_vmethods_using_this,
      m_staticize_dmethods_not_using_this, m_staticize_dmethods_using_this,
      m_ignore_keep, config.get_do_not_devirt_anon());
  const auto scope = build_class_scope(stores);
  const auto metrics = devirt.devirtualize_methods(scope);
  manager.incr_metric("num_staticized_methods_drop_this",
                      metrics.num_methods_not_using_this);
  manager.incr_metric("num_staticized_methods_keep_this",
                      metrics.num_methods_using_this);
  manager.incr_metric("num_virtual_calls_converted", metrics.num_virtual_calls);
  manager.incr_metric("num_direct_calls_converted", metrics.num_direct_calls);
  manager.incr_metric("num_super_calls_converted", metrics.num_super_calls);
}

static MethodDevirtualizationPass s_pass;
