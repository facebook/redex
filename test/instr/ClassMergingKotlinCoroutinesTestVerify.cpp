/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdlib>
#include <cstring>

#include "Show.h"
#include "verify/VerifyUtil.h"

TEST_F(PostVerify, SinkCommonCtorInvocation) {
  auto* cls_coroutine_1 = find_class_named(
      classes, "Lkotlin/sequences/SequencesKt__SequencesKt$flatMapIndexed$1;");

  // TODO(T242960112): Kotlin 2.2 changed flatMapIndexed$1's field layout from
  // 7 to 9 fields, altering its shape from (0,5,0,2,0,0,0) to (0,7,0,2,0,0,0).
  const char* kotlin_version = std::getenv("kotlin_language_version");
  bool is_kotlin_2_2_or_later =
      kotlin_version != nullptr && std::strcmp(kotlin_version, "2.2") >= 0;

  auto* cls_coroutine_2 =
      is_kotlin_2_2_or_later
          ? find_class_named(
                classes,
                "Lkotlin/sequences/SequencesKt___SequencesKt$runningFoldIndexed$1;")
          : find_class_named(
                classes,
                "Lkotlin/sequences/SequencesKt___SequencesKt$runningReduceIndexed$1;");

  verify_class_merged(cls_coroutine_1);
  verify_class_merged(cls_coroutine_2);
}
