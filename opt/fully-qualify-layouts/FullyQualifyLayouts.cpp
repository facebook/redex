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
  always_assert(!zip_dir.empty());
  auto resources = create_resource_reader(zip_dir);
  auto res_table = resources->load_res_table();

  auto type_ids = res_table->get_types_by_name_prefixes({"layout"});
  std::unordered_set<std::string> all_files;
  for (const auto& id : res_table->sorted_res_ids) {
    uint32_t type_id = id & TYPE_MASK_BIT;
    if (type_ids.count(type_id) > 0) {
      auto files = res_table->get_files_by_rid(id, ResourcePathType::ZipPath);
      if (!files.empty() && traceEnabled(RES, 8)) {
        TRACE(RES, 8, "ID 0x%x -> {", id);
        for (const auto& s : files) {
          TRACE(RES, 8, "  %s", s.c_str());
        }
        TRACE(RES, 8, "}");
      }
      all_files.insert(files.begin(), files.end());
    }
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
