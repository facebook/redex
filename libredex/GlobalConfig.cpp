/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "GlobalConfig.h"

void OptDecisionsConfig::bind_config() {
  bind("enable_logs", false, enable_logs,
       "Should we log Redex's optimization decisions?");
  bind("output_file_name", "", output_file_name,
       "Filename that optimization decisions will be logged too.");
}

void GlobalConfig::bind_config() {
  bind("opt_decisions", OptDecisionsConfig(), m_opt_decisions_config);
}
