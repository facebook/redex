/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "HasSourceBlocksChecker.h"

#include "ConfigFiles.h"
#include "Debug.h"
#include "DexClass.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Walkers.h"

namespace redex_properties {

#define WEAK_HAS_SOURCE_BLOCKS_CHECKER
void HasSourceBlocksChecker::run_checker(DexStoresVector& stores,
                                         ConfigFiles& config,
                                         PassManager&,
                                         bool) {
  // If InsertSourceBlocksPass is not on the pass list, or is disabled, don't
  // run this check.
  const auto& json_config = config.get_json_config();
  const auto& passes_from_config = json_config["redex"]["passes"];

  static constexpr const char* sb_passname = "InsertSourceBlocksPass";

  if (!std::any_of(
          passes_from_config.begin(), passes_from_config.end(),
          [](const auto& pass_name) { return pass_name == sb_passname; })) {
    return;
  }

  if (json_config.contains(sb_passname)) {
    const auto& pass_data = json_config[sb_passname];
    if (pass_data.isMember("disabled")) {
      if (pass_data["disabled"].asBool()) {
        return;
      }
    }
  }

  const auto& scope = build_class_scope(stores);

// The weak version checks that any source block exists at all.
// The strong versions checks that at least one source block exists per
// method. This is not clean right now.
// TODO: Make it clean.
#ifdef WEAK_HAS_SOURCE_BLOCKS_CHECKER
  std::atomic<bool> any_source_block_exists = false;
  walk::parallel::methods(scope, [&](DexMethod* method) {
    if (!method->get_code()) {
      return;
    }
    if (source_blocks::get_first_source_block_of_method(method)) {
      any_source_block_exists = true;
    }
  });
#else
  walk::parallel::methods(scope, [&](DexMethod* method) {
    if (!method->get_code()) {
      return;
    }

    always_assert_log(source_blocks::get_first_source_block_of_method(method),
                      "[%s] %s has no source blocks.\n",
                      get_property_name().c_str(), SHOW(method));
  });
#endif
}

} // namespace redex_properties

namespace {
static redex_properties::HasSourceBlocksChecker s_checker;
} // namespace
