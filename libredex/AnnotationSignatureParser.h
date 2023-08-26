/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexAnnotation.h"
#include "DexClass.h"

namespace annotation_signature_parser {

void parse(
    const DexAnnotation* anno,
    const std::function<bool(const DexEncodedValueString*, DexClass*)>& pred);

} // namespace annotation_signature_parser
