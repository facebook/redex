/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Show.h"
#include "verify/VerifyUtil.h"

TEST_F(PostVerify, SinkCommonCtorInvocation) {
  auto cls_coroutine_1 = find_class_named(
      classes, "Lkotlin/sequences/SequencesKt__SequencesKt$flatMapIndexed$1;");
  auto cls_coroutine_2 = find_class_named(
      classes,
      "Lkotlin/sequences/SequencesKt___SequencesKt$runningReduceIndexed$1;");

  verify_class_merged(cls_coroutine_1);
  verify_class_merged(cls_coroutine_2);
}
