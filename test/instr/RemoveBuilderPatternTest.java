/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.instr;

import org.junit.Test;
import static org.assertj.core.api.Assertions.assertThat;

class Model {
  public int field;

  public static Builder newBuilder() {
    return new Builder();
  }

  public static class Builder {
    int field = 0;

    public void set_field(int f) {
      field = f;
    }

    public Model build() {
      Model obj = new Model();
      obj.field = field;
      return obj;
    }
  }
}

public class RemoveBuilderPatternTest {

  @Test
  public void testRemoveSimpleBuilder() {
    Model.Builder builder = Model.newBuilder();
    int data = 1;
    builder.set_field(data);
    Model obj = builder.build();
    assertThat(obj.field).isEqualTo(1);
  }
}
