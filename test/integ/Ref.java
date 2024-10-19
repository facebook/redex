/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package kotlin.jvm.internal;

import java.io.Serializable;

public class Ref {
  private Ref() {}
  
    public static final class ObjectRef<T> implements Serializable {
        public T element;

        @Override
        public String toString() {
            return String.valueOf(element);
        }
    }

  public static final class IntRef implements Serializable {
        public int element;

        @Override
        public String toString() {
            return String.valueOf(element);
        }
    }
}
