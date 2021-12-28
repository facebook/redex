/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Model.h"

namespace class_merging {

void merge_model(Scope& scope,
                 ConfigFiles& conf,
                 PassManager& mgr,
                 DexStoresVector& stores,
                 ModelSpec& spec);

void merge_model(const TypeSystem&,
                 Scope& scope,
                 ConfigFiles& conf,
                 PassManager& mgr,
                 DexStoresVector& stores,
                 ModelSpec& spec);

} // namespace class_merging
