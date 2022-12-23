/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ApkResources.h"
#include "RedexResources.h"
#include "verify/VerifyUtil.h"

void preverify_impl(const DexClasses& classes, ResourceTableFile* res_table);
void postverify_impl(const DexClasses& classes, ResourceTableFile* res_table);
void preverify_nullify_impl(const DexClasses& classes,
                            ResourceTableFile* res_table);
void postverify_nullify_impl(const DexClasses& classes,
                             ResourceTableFile* res_table);
void apk_postverify_impl(ResourcesArscFile* res_table);
void apk_postverify_nullify_impl(ResourcesArscFile* res_table);
