/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.examples.proguardexample

class Alpha : Greek {
  private val wombat = 21

  override fun doubleWombat(): Int {
    return 2 * wombat
  }
}
