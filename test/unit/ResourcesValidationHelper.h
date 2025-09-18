/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "RedexResources.h"

// Validation functions that are expected to behave identically between apk/aab
// inputs.
void validate_walk_references_for_resource(ResourceTableFile* res_table);
