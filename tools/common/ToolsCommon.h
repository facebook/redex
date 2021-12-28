/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConfigFiles.h"
#include "DexStats.h"
#include "DexStore.h"
#include "PassManager.h"

namespace redex {

bool dir_is_writable(const std::string& dir);

Json::Value parse_config(const std::string& config_file);

void write_all_intermediate(ConfigFiles& conf,
                            const std::string& output_ir_dir,
                            const RedexOptions& redex_options,
                            DexStoresVector& stores,
                            Json::Value& entry_data);

void load_all_intermediate(const std::string& input_ir_dir,
                           DexStoresVector& stores,
                           Json::Value* entry_data);

void load_classes_from_dexes_and_metadata(
    const std::vector<std::string>& dex_files,
    DexStoresVector& stores,
    dex_stats_t& input_totals,
    std::vector<dex_stats_t>& input_dexes_stats);

std::string get_dex_output_name(const std::string& output_dir,
                                const DexStore& store,
                                int index);
} // namespace redex
