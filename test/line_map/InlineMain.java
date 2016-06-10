/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redexlinemap;

import static org.fest.assertions.api.Assertions.*;

import org.junit.Test;

public class InlineMain {
  /**
   * Check that the stack trace of a non-inlined function makes sense.
   */
  @Test
  public void testBasic() throws Exception {
    InlineSeparateFile.wrapsThrow();
  }
  /**
   * Check the stack trace of a once-inlined function.
   */
  @Test
  public void testInlineOnce() throws Exception {
    InlineSeparateFile.inlineMe();
  }
  /**
   * Check the stack trace of a multiply-inlined function.
   */
  @Test
  public void testNested() throws Exception {
    InlineSeparateFile.inlineNested();
  }
}
