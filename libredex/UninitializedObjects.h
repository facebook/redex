/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>

#include <sparta/ConstantAbstractDomain.h>
#include <sparta/PatriciaTreeMapAbstractEnvironment.h>

#include "DexClass.h"
#include "IRInstruction.h"

namespace uninitialized_objects {

using UninitializedObjectDomain = sparta::ConstantAbstractDomain<bool>;

using UninitializedObjectEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<reg_t,
                                               UninitializedObjectDomain>;

using UninitializedObjectEnvironments =
    std::unordered_map<const IRInstruction*, UninitializedObjectEnvironment>;

// For each instruction, ånd each incoming register, determine if it may contain
// an uninitialized object, i.e. one that was created or passed in via the
// receiver argument of a constructor, and no invoke-direct to a constructor
// happened yet.
UninitializedObjectEnvironments get_uninitialized_object_environments(
    DexMethod* method);

} // namespace uninitialized_objects
