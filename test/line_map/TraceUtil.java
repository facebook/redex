/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redexlinemap;

import java.util.ArrayList;
import java.util.List;

public class TraceUtil {
  /**
   * Stringifies and returns the top N elements of a stack trace.
   */
  static public ArrayList<String> traceToString(
      List<StackTraceElement> trace, int limit) {
    ArrayList<String> result = new ArrayList();
    for (int i = 0; i < limit && i < trace.size(); ++i) {
      result.add(trace.get(i).toString());
    }
    return result;
  }

}
