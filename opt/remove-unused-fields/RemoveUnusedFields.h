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
 * to them. Similarly, all fields that are never written to and do not have
 * a non-zero static value get all of their read instructions replaced by
 * const 0 instructions.
 *
 * This pass relies on RemoveUnreachablePass running afterward to remove the
 * definitions of those fields entirely.
 *
 * Possible future work: This could be extended to eliminate fields that are
 * only used in non-escaping contexts.
 *
 * NOTE: Removing writes to fields may affect the life-time of an object, if all
 * other references to it are weak. Thus, this is a somewhat unsafe, or at least
 * potentially behavior altering optimization.
 */
namespace remove_unused_fields {

class PassImpl : public Pass {
 public:
  PassImpl() : Pass("RemoveUnusedFieldsPass") {}

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;
};

} // namespace remove_unused_fields
