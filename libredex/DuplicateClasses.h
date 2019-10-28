/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexClass.h"
#include "JsonWrapper.h"

namespace dup_classes {

// Read whitelisted duplicate class list from config.
void read_dup_class_whitelist(const JsonWrapper& json_cfg);

// Return true if the cls is among one of the known whitelisted duplicated
// classes.
bool is_known_dup(DexClass* cls);

} // namespace dup_classes
