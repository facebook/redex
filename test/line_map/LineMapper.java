/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redexlinemap;

import java.io.*;
import java.util.ArrayList;

public class LineMapper {
  ArrayList<Position> mapping;

  public LineMapper(InputStream ins) throws IOException {
    mapping = new ArrayList();
    BufferedReader reader = new BufferedReader(new InputStreamReader(ins));
    String input;
    while ((input = reader.readLine()) != null) {
      String[] parts = input.split("\\|");
      int parent = Integer.parseInt(parts[1]) - 1;
      String[] parts_ = parts[0].split(":");
      int line = Integer.parseInt(parts_[1]);
      mapping.add(new Position(parts_[0], line, parent));
    }
  }

  public ArrayList<Position> getPositionsAt(int idx) {
    ArrayList<Position> positions = new ArrayList();
    while (idx >= 0) {
      Position pos = mapping.get(idx);
      positions.add(pos);
      idx = pos.parent;
    }
    return positions;
  }

  public ArrayList<StackTraceElement> mapStackTrace(StackTraceElement[] trace) {
    ArrayList<StackTraceElement> newTrace = new ArrayList();
    for (StackTraceElement el : trace) {
      String fn = el.getFileName();
      if (fn.equals("")) {
        int line = el.getLineNumber();
        ArrayList<Position> positions = getPositionsAt(line - 1);
        for (Position pos : positions) {
          newTrace.add(new StackTraceElement(
            el.getClassName(), el.getMethodName(), pos.file, pos.line));
        }
      } else {
        newTrace.add(el);
      }
    }
    return newTrace;
  }
}

class Position {
  String file;
  int line;
  int parent;
  public Position(String file, int line, int parent) {
    this.file = file;
    this.line = line;
    this.parent = parent;
  }
}
