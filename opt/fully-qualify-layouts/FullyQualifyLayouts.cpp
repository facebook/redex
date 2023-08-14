/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "FullyQualifyLayouts.h"

#include <cinttypes>
#include <unordered_map>

#include "ConfigFiles.h"
#include "RedexResources.h"
#include "Trace.h"
#include "WorkQueue.h"

namespace {
constexpr const char* METRIC_CHANGED_ELEMENTS = "fully_qualified_elements";

std::unordered_map<std::string, std::string> KNOWN_ELEMENTS = {
    {"SurfaceView", "android.view.SurfaceView"},
    {"TextureView", "android.view.TextureView"},
    {"View", "android.view.View"},
    {"ViewStub", "android.view.ViewStub"},
    {"WebView", "android.webkit.WebView"}};
} // namespace

void FullyQualifyLayouts::run_pass(DexStoresVector& /* unused */,
                                   ConfigFiles& conf,
                                   PassManager& mgr) {
  std::string zip_dir;
  conf.get_json_config().get("apk_dir", "", zip_dir);
  always_assert(zip_dir.size());
  auto resources = create_resource_reader(zip_dir);
  auto res_table = resources->load_res_table();

  auto ids = res_table->get_res_ids_by_type_name("layout");
  std::unordered_set<std::string> all_files;
  for (const auto& id : ids) {
    auto files = res_table->get_files_by_rid(id, ResourcePathType::ZipPath);
    all_files.insert(files.begin(), files.end());
  }

  workqueue_run<std::string>(
      [&](sparta::WorkerState<std::string>* /* unused */,
          const std::string& file_path) {
        size_t changes{0};
        resources->fully_qualify_layout(
            KNOWN_ELEMENTS, zip_dir + "/" + file_path, &changes);
        if (changes > 0) {
          TRACE(RES,
                8,
                "Updated %zu element(s) in %s",
                changes,
                file_path.c_str());
        }
        mgr.incr_metric(METRIC_CHANGED_ELEMENTS, changes);
      },
      all_files);
  TRACE(RES,
        2,
        "%" PRId64 " element(s) modified",
        mgr.get_metric(METRIC_CHANGED_ELEMENTS));
}

static FullyQualifyLayouts s_pass;
