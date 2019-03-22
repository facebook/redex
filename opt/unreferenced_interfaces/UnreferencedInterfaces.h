/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "PassManager.h"
#include "DexClass.h"

/**
 * Remove internal interfaces that have no reference anywhere in code
 * except that in an 'implements' clause.
 * If there are no references in code the interface can be safely removed.
 */
class UnreferencedInterfacesPass : public Pass {
 public:
  UnreferencedInterfacesPass() : Pass("UnreferencedInterfacesPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  struct Metric {
    size_t candidates{0};
    size_t external{0};
    size_t on_abstract_cls{0};
    size_t field_refs{0};
    size_t sig_refs{0};
    size_t insn_refs{0};
    size_t anno_refs{0};
    size_t unresolved_meths{0};
    size_t updated_impls{0};
    size_t removed{0};
  } m_metric;
};
