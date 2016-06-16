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
import java.util.Map;
import com.fasterxml.jackson.databind.ObjectMapper;

public class LineMapper {
  ArrayList<Position> mapping;

  public LineMapper(InputStream ins) throws IOException {
    mapping = new ArrayList();
    BufferedReader reader = new BufferedReader(new InputStreamReader(ins));
    String input;
    ObjectMapper json = new ObjectMapper();
    while ((input = reader.readLine()) != null) {
      Map<String, Object> data = json.readValue(input, Map.class);
      mapping.add(new Position((String)data.get("file"),
                               (Integer)data.get("line"),
                               (Integer)data.get("parent") - 1));
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
