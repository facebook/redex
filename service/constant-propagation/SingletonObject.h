/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConstantAbstractDomain.h"
#include "DexClass.h"

/*
 * This represents an object that is uniquely referenced by a single static
 * field. This enables us to compare these objects easily -- we can determine
 * whether two different SingletonObjectDomain elements are equal just based
 * on their representation in the abstract environment, without needing to
 * check if they are pointing to the same object in the abstract heap.
 */
using SingletonObjectDomain = sparta::ConstantAbstractDomain<const DexField*>;
