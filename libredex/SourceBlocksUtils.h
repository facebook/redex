/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "IRList.h"

class DexMethod;

namespace source_blocks {

std::unique_ptr<SourceBlock> clone_as_synthetic(SourceBlock* sb,
                                                const DexMethod* ref,
                                                const SourceBlock::Val& val);

std::unique_ptr<SourceBlock> clone_as_synthetic(
    SourceBlock* sb,
    const DexMethod* ref = nullptr,
    const std::optional<SourceBlock::Val>& opt_val = std::nullopt);

std::unique_ptr<SourceBlock> clone_as_synthetic(
    SourceBlock* sb,
    const DexMethod* ref,
    const std::vector<SourceBlock*>& many);

} // namespace source_blocks
