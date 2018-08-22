/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConfigFiles.h"
#include "DexStore.h"

void write_ir_meta(const std::string& output_ir_dir, DexStoresVector& stores);

void write_intermediate_dex(const ConfigFiles& cfg,
                            const std::string& output_ir_dir,
                            DexStoresVector& stores);
