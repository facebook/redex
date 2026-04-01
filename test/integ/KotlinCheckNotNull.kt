/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

class KotlinCheckNotNull {
  // !! operator generates checkNotNull(Object)V
  fun notNullAssert(x: Any?): Any = x!!

  // as cast to non-null type generates checkNotNull(Object, String)V
  fun castToNonNull(x: Any?): String = x as String
}
