/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Model.h"

namespace class_merging {

void set_up(ConfigFiles& conf);

void merge_model(Scope& scope,
                 PassManager& mgr,
                 DexStoresVector& stores,
                 ModelSpec& spec);

} // namespace class_merging
