/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexlinemap;

import java.util.ArrayList;
import java.util.List;

public class TraceUtil {
  /** Stringifies and returns the top N elements of a stack trace. */
  public static ArrayList<String> traceToString(List<StackTraceElement> trace, int limit) {
    ArrayList<String> result = new ArrayList();
    for (int i = 0; i < limit && i < trace.size(); ++i) {
      result.add(trace.get(i).toString().replace(":0", ""));
    }
    return result;
  }
}
