/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ResourceValueMergingPass.h"
#include "ConfigFiles.h"
#include "DeterministicContainers.h"
#include "Pass.h"
#include "PassManager.h"
#include "RedexResources.h"
#include "Styles.h"
#include "Trace.h"

void ResourceValueMergingPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& conf,
                                        PassManager& mgr) {
  TRACE(RES, 1, "ResourceValueMergingPass excluded_resources count: %zu",
        m_excluded_resources.size());

  for (const auto& resource : UnorderedIterable(m_excluded_resources)) {
    TRACE(RES, 1, "  Excluded resource: %s", resource.c_str());
  }

  std::string apk_dir;
  conf.get_json_config().get("apk_dir", "", apk_dir);

  auto resources = create_resource_reader(apk_dir);
  auto res_table = resources->load_res_table();
  auto style_info = res_table->load_style_info();
  resources::ReachabilityOptions options;
  StyleAnalysis style_analysis(apk_dir, conf.get_global_config(), options,
                               stores, UnorderedSet<uint32_t>());
  std::string style_dot = style_analysis.dot(false, true);
  TRACE(RES, 1, "StyleAnalysis dot output:\n%s", style_dot.c_str());
}

static ResourceValueMergingPass s_pass;
