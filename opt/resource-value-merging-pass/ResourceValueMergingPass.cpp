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
  auto dot_graph = style_info.print_as_dot(false);

  TRACE(RES, 1, "Style graph in DOT format:\n%s", dot_graph.c_str());
  TRACE(RES, 1, "Style count: %zu", style_info.styles.size());
}

static ResourceValueMergingPass s_pass;
