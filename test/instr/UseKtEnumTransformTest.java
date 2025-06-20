/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import android.annotation.SuppressLint;
import javax.annotation.Nullable;
import org.junit.Test;
import static org.assertj.core.api.Assertions.assertThat;

public class UseKtEnumTransformTest {

  D getD() {
    return D.D0;
  }

  F getF() {
    return F.F0;
  }

  void sayHello(String s) {
    System.err.println(s);
  }

  @Test
  public void testStringConcat() {
    D d = null;
    if (System.currentTimeMillis() > 0) {
      d = getD();
    }
    sayHello("Hello "
                  + (d != null
                      ? d
                      : "unknown"));
  }

  @SuppressLint("DeadVariable")
  @Test
  public void testException() {
    F f = null;
    if (System.currentTimeMillis() > 0) {
      f = getF();
    }
    try {
      String s = String.valueOf(System.err);
    } catch (Exception e) {
      StringBuilder sb = new StringBuilder("Hello ");
      sb.append(f != null ? f : e);
      sayHello(sb.toString());
    }
  }
}
