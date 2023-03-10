/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.instr;

import static org.assertj.core.api.Assertions.*;
import java.lang.reflect.*;

import org.junit.Test;

@interface OriginalNameAnnotation {
  String value();
};

class Parameterized<T> {}

class ExtendsParameterized extends Parameterized<RenameClassesTest> {}

public class RenameClassesTest {

  @Test
  public void testIAmRenamed() {
    assertThat(this.getClass().getName().startsWith("X.")).isTrue();
  }

  @Test
  public void testSignatureAnnoRewrite() throws Exception {
    ParameterizedType ptype =
        (ParameterizedType)ExtendsParameterized.class.getGenericSuperclass();
    assertThat(ptype.getActualTypeArguments()[0]).isEqualTo(this.getClass());
  }

  @Test
  public void testOriginalNameAnnotation() throws Exception {
    assertThat((String) this.getClass()
                   .getDeclaredField("__redex_internal_original_name")
                   .get(this))
      .isEqualTo(
        Utils.demangle("RenameClassesTest"));
  }

  @Test
  public void testDescriptorLikeStrings() {
    StringBuilder sb = new StringBuilder();
    sb.append("LX/7K;");
    sb.append("123");
    assertThat(sb.toString().length()).isEqualTo(9);
  }
}
