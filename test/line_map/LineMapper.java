/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexlinemap;

import java.io.*;
import java.util.ArrayList;

public class LineMapper {
  String[] stringPool;
  ArrayList<Position> mapping;

  private int readInt(DataInputStream ds) throws IOException {
    // DataInputStream's methods assume big-endianess, so we have to write our
    // own integer reader
    byte[] b = new byte[4];
    ds.readFully(b, 0, 4);
    return (b[0] & 0xff) | ((b[1] & 0xff) << 8) | ((b[2] & 0xff) << 16) |
        ((b[3] & 0xff) << 24);
  }

  public LineMapper(InputStream ins) throws Exception {
    mapping = new ArrayList();
    DataInputStream ds = new DataInputStream(ins);
    long magic = readInt(ds);
    if (magic != 0xfaceb000) {
      throw new Exception("Magic number mismatch: got " +
                          Long.toHexString(magic));
    }
    int version = readInt(ds);
    if (version != 1) {
      throw new Exception("Version mismatch");
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
      int file_id = readInt(ds);
      int line = readInt(ds);
      long parent = readInt(ds);
      mapping.add(new Position(file_id, line, parent));
    }
  }

  public ArrayList<Position> getPositionsAt(long idx) {
    ArrayList<Position> positions = new ArrayList();
    while (idx >= 0) {
      Position pos = mapping.get((int)idx);
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
        ArrayList<Position> positions = getPositionsAt(line - 1);
        for (Position pos : positions) {
          newTrace.add(new StackTraceElement(el.getClassName(),
                                             el.getMethodName(),
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

class Position {
  int file_id;
  int line;
  long parent;
  public Position(int file_id, int line, long parent) {
    this.file_id = file_id;
    this.line = line;
    this.parent = parent;
  }
}
