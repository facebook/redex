/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConfigFiles.h"
#include "DexStore.h"
#include "PassManager.h"

namespace redex {

bool dir_is_writable(const std::string& dir);

Json::Value parse_config(const std::string& config_file);

void write_all_intermediate(const ConfigFiles& conf,
                            const std::string& output_ir_dir,
                            const RedexOptions& redex_options,
                            DexStoresVector& stores,
                            Json::Value& entry_data);

void load_all_intermediate(const std::string& input_ir_dir,
                           DexStoresVector& stores,
                           Json::Value* entry_data);
} // namespace redex
