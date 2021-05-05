/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

fun <T> lazyFn(initializer: () -> T): Lazy<T> = Foo(initializer)

internal class Foo<out T>(initializer: () -> T) : Lazy<T> {
  private val unsafeLazy = lazy(LazyThreadSafetyMode.NONE, initializer)
  override val value: T
    get() {
      return unsafeLazy.value
    }

  override fun isInitialized(): Boolean = unsafeLazy.isInitialized()
}
