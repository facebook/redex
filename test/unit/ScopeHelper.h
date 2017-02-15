/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexClass.h"
#include "DexAccess.h"

using Scope = std::vector<DexClass*>;

/**
 * Gives you an scope initialized with java.lang.Object.
 * Builds the DexClass for java.lang.Object.
 */
Scope create_empty_scope();

/**
 * Create a DexClass with the given specification.
 * The class is marked internal and available to redex for optimizations.
 */
DexClass* create_internal_class(
    DexType* type,
    DexType* super,
    std::vector<DexType*> interfaces,
    DexAccessFlags access = ACC_PUBLIC);

/**
 * Create a DexClass with the given specification.
 * The class is marked external as a library or system class for which
 * we have the DexClass.
 */
DexClass* create_external_class(
    DexType* type,
    DexType* super,
    std::vector<DexType*> interfaces,
    DexAccessFlags access = ACC_PUBLIC);

/**
 * Add an abstract method to the given class.
 */
DexMethod* create_abstract_method(
    DexClass* cls,
    const char* name,
    DexProto* proto,
    DexAccessFlags access = ACC_PUBLIC);

/**
 * Add a concrete empty method (only return statement) to the given class.
 */
DexMethod* create_empty_method(
    DexClass* cls,
    const char* name,
    DexProto* proto,
    DexAccessFlags access = ACC_PUBLIC);
