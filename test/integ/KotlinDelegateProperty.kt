/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

import kotlin.reflect.KProperty

class Delegate1 {
  operator fun getValue(thisRef: Any?, property: KProperty<*>): String {
    return "$thisRef, '${property.name}'"
  }

  operator fun setValue(thisRef: Any?, property: KProperty<*>, value: String) {
    println("$value '${property.name}' $thisRef")
  }
}

class Example {
  var p: String by Delegate1()
}
