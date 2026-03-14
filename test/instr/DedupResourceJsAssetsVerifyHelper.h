/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexAnnotation.h"
#include "RedexResources.h"
#include "verify/VerifyUtil.h"

// Verifies that DedupResourcesPass respects used-js-assets by not deduping
// resources that are referenced from JavaScript (resolved by name at runtime).
//
// Test setup:
// - icon.png, x_icon.png, js_referenced_icon.png are all identical files
// - js_referenced_icon is listed in used-js-assets (test_js_assets.json)
// - icon and x_icon are NOT in used-js-assets
//
// Expected behavior:
// - icon and x_icon should be deduped (same ID after pass)
// - js_referenced_icon should NOT be deduped (retains its own ID and name)

inline void dedupresource_js_assets_preverify(const DexClasses& classes,
                                              ResourceTableFile* res_table) {
  auto* drawable_cls = find_class_named(classes, "Lcom/facebook/R$drawable;");
  ASSERT_NE(nullptr, drawable_cls);

  auto* icon = find_sfield_named(*drawable_cls, "icon");
  auto* x_icon = find_sfield_named(*drawable_cls, "x_icon");
  auto* js_icon = find_sfield_named(*drawable_cls, "js_referenced_icon");
  ASSERT_NE(nullptr, icon);
  ASSERT_NE(nullptr, x_icon);
  ASSERT_NE(nullptr, js_icon);

  // Before dedup, all three have different IDs
  EXPECT_NE(icon->get_static_value()->value(),
            x_icon->get_static_value()->value());
  EXPECT_NE(icon->get_static_value()->value(),
            js_icon->get_static_value()->value());
  EXPECT_NE(x_icon->get_static_value()->value(),
            js_icon->get_static_value()->value());

  // All three should be resolvable by name
  auto icon_ids = res_table->get_res_ids_by_name("icon");
  auto x_icon_ids = res_table->get_res_ids_by_name("x_icon");
  auto js_icon_ids = res_table->get_res_ids_by_name("js_referenced_icon");
  EXPECT_EQ(icon_ids.size(), 1);
  EXPECT_EQ(x_icon_ids.size(), 1);
  EXPECT_EQ(js_icon_ids.size(), 1);
}

inline void dedupresource_js_assets_postverify(const DexClasses& classes,
                                               ResourceTableFile* res_table) {
  auto* drawable_cls = find_class_named(classes, "Lcom/facebook/R$drawable;");
  ASSERT_NE(nullptr, drawable_cls);

  auto* icon = find_sfield_named(*drawable_cls, "icon");
  auto* x_icon = find_sfield_named(*drawable_cls, "x_icon");
  auto* js_icon = find_sfield_named(*drawable_cls, "js_referenced_icon");
  ASSERT_NE(nullptr, icon);
  ASSERT_NE(nullptr, x_icon);
  ASSERT_NE(nullptr, js_icon);

  // icon and x_icon should be deduped (same ID) — normal dedup still works
  EXPECT_EQ(icon->get_static_value()->value(),
            x_icon->get_static_value()->value());

  // js_referenced_icon must NOT be deduped into icon — it needs its own ID
  // so getIdentifier("js_referenced_icon") works at runtime
  EXPECT_NE(icon->get_static_value()->value(),
            js_icon->get_static_value()->value());

  // js_referenced_icon must still be resolvable by name
  auto js_icon_ids = res_table->get_res_ids_by_name("js_referenced_icon");
  EXPECT_EQ(js_icon_ids.size(), 1)
      << "js_referenced_icon should not be deduped because it is in "
         "used-js-assets";
}
