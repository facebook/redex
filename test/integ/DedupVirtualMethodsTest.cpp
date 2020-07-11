/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DedupVirtualMethods.h"
#include "AnnoUtils.h"
#include "RedexTest.h"

class DedupVirtualMethodsTest : public RedexIntegrationTest {};

struct VMethodsStats {
  uint32_t total{0};
  uint32_t annotated{0};
};

VMethodsStats count_methods(const Scope& scope, DexType* annotation) {
  VMethodsStats stats;
  for (auto cls : scope) {
    for (auto method : cls->get_vmethods()) {
      stats.total++;
      stats.annotated +=
          (((get_annotation(method, annotation) == nullptr)) ? 0 : 1);
    }
  }
  return stats;
}

TEST_F(DedupVirtualMethodsTest, dedup) {
  auto scope = build_class_scope(stores);
  auto annotation = DexType::get_type("Lcom/facebook/redextest/Duplication;");

  auto before_stats = count_methods(scope, annotation);
  auto deduplicated_vmethods = dedup_vmethods::dedup(stores);
  auto after_stats = count_methods(scope, annotation);

  EXPECT_EQ(after_stats.annotated, 0);
  EXPECT_EQ(deduplicated_vmethods, before_stats.annotated);
  EXPECT_EQ(before_stats.total - before_stats.annotated, after_stats.total);
}
