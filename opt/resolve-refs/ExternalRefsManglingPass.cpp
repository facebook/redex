/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ExternalRefsManglingPass.h"

#include "ConfigFiles.h"
#include "PassManager.h"
#include "Trace.h"

void ExternalRefsManglingPass::eval_pass(DexStoresVector&,
                                         ConfigFiles& conf,
                                         PassManager& mgr) {
  int32_t min_sdk = mgr.get_redex_options().min_sdk;
  // Disable refinement to external for API level older than
  // `m_supported_min_sdk_for_external_refs`.
  if (m_refine_to_external && min_sdk < m_supported_min_sdk_for_external_refs) {
    m_refine_to_external = false;
    TRACE(PM, 2, "Disabling refinement to external for min_sdk %d", min_sdk);
  }

  // Load min_sdk API file
  auto min_sdk_api_file = conf.get_android_sdk_api_file(min_sdk);
  if (!min_sdk_api_file) {
    TRACE(PM, 2, "Android SDK API %d file cannot be found.", min_sdk);
    always_assert_log(
        !m_refine_to_external ||
            min_sdk < m_supported_min_sdk_for_external_refs,
        "Android SDK API %d file can not be found but resolve_to_external is "
        "explicitly enabled for this version. Please pass the api list to "
        "Redex or turn off `resolve_to_external`.",
        min_sdk);
    m_refine_to_external = false;
  } else {
    TRACE(PM, 2, "Android SDK API %d file found: %s", min_sdk,
          min_sdk_api_file->c_str());
  }

  m_min_sdk_api = &conf.get_android_sdk_api(min_sdk);
}
