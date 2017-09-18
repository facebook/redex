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

public class LineMapperV2 {
  String[] stringPool;
  ArrayList<PositionV2> mapping;

  private int readInt(DataInputStream ds) throws IOException {
    // DataInputStream's methods assume big-endianess, so we have to write our
    // own integer reader
    byte[] b = new byte[4];
    ds.readFully(b, 0, 4);
    return (b[0] & 0xff) | ((b[1] & 0xff) << 8) | ((b[2] & 0xff) << 16) |
        ((b[3] & 0xff) << 24);
  }

  public LineMapperV2(InputStream ins) throws Exception {
    mapping = new ArrayList();
    DataInputStream ds = new DataInputStream(ins);
    long magic = readInt(ds);
    if (magic != 0xfaceb000) {
      throw new Exception("Magic number mismatch: got " +
                          Long.toHexString(magic));
    }
    int version = readInt(ds);
    if (version != 2) {
      throw new Exception("Version mismatch: Expected 2, got " +
                          Integer.toString(version));
    }
    int spool_count = readInt(ds);
    System.out.println(Integer.toString(spool_count));
    stringPool = new String[spool_count];
    for (int i = 0; i < spool_count; ++i) {
      int ssize = readInt(ds);
      byte[] bytes = new byte[ssize];
      ds.readFully(bytes, 0, ssize);
      stringPool[i] = new String(bytes);
    }
    int pos_count = readInt(ds);
    for (int i = 0; i < pos_count; ++i) {
      int class_id = readInt(ds);
      int method_id = readInt(ds);
      int file_id = readInt(ds);
      int line = readInt(ds);
      long parent = readInt(ds);
      mapping.add(new PositionV2(class_id, method_id, file_id, line, parent));
    }
  }

  public ArrayList<PositionV2> getPositionsAt(long idx) {
    ArrayList<PositionV2> positions = new ArrayList();
    while (idx >= 0) {
      PositionV2 pos = mapping.get((int)idx);
      positions.add(pos);
      idx = pos.parent - 1;
    }
    return positions;
  }

  public ArrayList<StackTraceElement> mapStackTrace(StackTraceElement[] trace) {
    ArrayList<StackTraceElement> newTrace = new ArrayList();
    for (StackTraceElement el : trace) {
      String fn = el.getFileName();
      if (fn.equals("")) {
        int line = el.getLineNumber();
        ArrayList<PositionV2> positions = getPositionsAt(line - 1);
        for (PositionV2 pos : positions) {
          newTrace.add(new StackTraceElement(stringPool[pos.class_id],
                                             stringPool[pos.method_id],
                                             stringPool[pos.file_id],
                                             pos.line));
        }
      } else {
        newTrace.add(el);
      }
    }
    return newTrace;
  }
}

class PositionV2 {
  int class_id;
  int method_id;
  int file_id;
  int line;
  long parent;
  public PositionV2(
      int class_id, int method_id, int file_id, int line, long parent) {
    this.class_id = class_id;
    this.method_id = method_id;
    this.file_id = file_id;
    this.line = line;
    this.parent = parent;
  }
}
