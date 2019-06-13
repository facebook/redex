/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

/*
 * This pass identifies fields that are never read from and deletes all writes
 * to them. It relies on RemoveUnreachablePass running afterward to remove the
 * definitions of those fields entirely.
 *
 * Possible future work: This could be extended to eliminate fields that are
 * only used in non-escaping contexts.
 */
namespace remove_unread_fields {

class PassImpl : public Pass {
 public:
  PassImpl() : Pass("RemoveUnreadFieldsPass") {}

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;
};

} // namespace remove_unread_fields
