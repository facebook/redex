/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexlinemap;

import java.io.DataInputStream;
import java.io.InputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;

public class LineMapperV2 {
  String[] stringPool;
  ArrayList<PositionV2> mapping;
  HashMap<String, Long> iodiMapping;
  HashMap<Long, ArrayList<OffsetLine>> lineMappings;

  private static int readShort(DataInputStream ds) throws IOException {
    // DataInputStream's methods assume big-endianess, so we have to write our
    // own integer reader
    byte[] b = new byte[2];
    ds.readFully(b, 0, 2);
    return (b[0] & 0xff) | ((b[1] & 0xff) << 8);
  }

  private static int readInt(DataInputStream ds) throws IOException {
    // DataInputStream's methods assume big-endianess, so we have to write our
    // own integer reader
    byte[] b = new byte[4];
    ds.readFully(b, 0, 4);
    return (b[0] & 0xff) | ((b[1] & 0xff) << 8) | ((b[2] & 0xff) << 16) | ((b[3] & 0xff) << 24);
  }

  private static long readLong(DataInputStream ds) throws IOException {
    // DataInputStream's methods assume big-endianess, so we have to write our
    // own integer reader
    byte[] b = new byte[8];
    ds.readFully(b, 0, 8);
    return (b[0] & 0xffL)
        | ((b[1] & 0xffL) << 8)
        | ((b[2] & 0xffL) << 16)
        | ((b[3] & 0xffL) << 24)
        | ((b[4] & 0xffL) << 32)
        | ((b[5] & 0xffL) << 40)
        | ((b[6] & 0xffL) << 48)
        | ((b[7] & 0xffL) << 56);
  }

  private static String readString(DataInputStream ds, int size) throws IOException {
    byte[] bytes = new byte[size];
    ds.readFully(bytes, 0, size);
    return new String(bytes);
  }

  public LineMapperV2(InputStream ins) throws Exception {
    readMapping(ins);
  }

  public LineMapperV2(InputStream ins, InputStream iodiIns, InputStream debugIns) throws Exception {
    readMapping(ins);
    readIodi(iodiIns);
    readDebugMap(debugIns);
  }

  private static class MethodData {
    long id;
    int i1, len;

    public MethodData(long l, int i1, int i2) {
      this.id = l;
      this.i1 = i1;
      this.len = i2;
    }
  }

  private static class OffsetLine {
    int pc, mappedLine;

    public OffsetLine(int i1, int i2) {
      this.pc = i1;
      this.mappedLine = i2;
    }
  }

  private void readDebugMap(InputStream ins) throws Exception {
    DataInputStream ds = new DataInputStream(ins);
    long magic = readInt(ds);
    if (magic != 0xfaceb000) {
      throw new IllegalArgumentException("Magic number mismatch: got " + Long.toHexString(magic));
    }
    int version = readInt(ds);
    if (version != 1) {
      throw new IllegalArgumentException(
          "Version mismatch: Expected 2, got " + Integer.toString(version));
    }
    int count = readInt(ds);

    ArrayList<MethodData> methodData = new ArrayList<>();
    for (int i = 0; i != count; i++) {
      methodData.add(new MethodData(readLong(ds), readInt(ds), readInt(ds)));
    }

    lineMappings = new HashMap<>();
    for (int i = 0; i != count; i++) {
      long methodId = readLong(ds);
      MethodData md = methodData.get(i);
      if (methodId != md.id) {
        throw new IllegalArgumentException("ID mismatch: " + methodId + " vs " + md.id);
      }
      if (lineMappings.containsKey(methodId)) {
        throw new IllegalArgumentException("Duplicate ID " + methodId);
      }
      int lineMappingSize = md.len - 8;
      int lineMappingCount = lineMappingSize / 8;
      if (lineMappingCount > 0) {
        ArrayList<OffsetLine> methodLineMappings = new ArrayList<>();
        for (int j = 0; j != lineMappingCount; j++) {
          methodLineMappings.add(new OffsetLine(readInt(ds), readInt(ds)));
        }
        lineMappings.put(methodId, methodLineMappings);
      }
    }
  }

  private void readIodi(InputStream ins) throws Exception {
    DataInputStream ds = new DataInputStream(ins);
    long magic = readInt(ds);
    if (magic != 0xfaceb001) {
      throw new IllegalArgumentException("Magic number mismatch: got " + Long.toHexString(magic));
    }
    int version = readInt(ds);
    if (version != 1) {
      throw new IllegalArgumentException(
          "Version mismatch: Expected 2, got " + Integer.toString(version));
    }
    int count = readInt(ds);
    int zero = readInt(ds);
    if (zero != 0) {
      throw new IllegalArgumentException("Expected 0, got " + zero);
    }

    iodiMapping = new HashMap<>();
    for (int i = 0; i != count; i++) {
      int stringLength = readShort(ds);
      long methodId = readLong(ds);
      String name = readString(ds, stringLength);
      iodiMapping.put(name, methodId);
    }
    if (ds.available() > 0) {
      throw new IllegalArgumentException("Bytes remaining: " + ds.available());
    }
  }

  private void readMapping(InputStream ins) throws Exception {
    mapping = new ArrayList();
    DataInputStream ds = new DataInputStream(ins);
    long magic = readInt(ds);
    if (magic != 0xfaceb000) {
      throw new IllegalArgumentException("Magic number mismatch: got " + Long.toHexString(magic));
    }
    int version = readInt(ds);
    if (version != 2) {
      throw new IllegalArgumentException(
          "Version mismatch: Expected 2, got " + Integer.toString(version));
    }
    int spool_count = readInt(ds);
    System.out.println(Integer.toString(spool_count));
    stringPool = new String[spool_count];
    for (int i = 0; i < spool_count; ++i) {
      int ssize = readInt(ds);
      stringPool[i] = readString(ds, ssize);
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
    ArrayList<PositionV2> positions = new ArrayList<>();
    while (idx >= 0) {
      PositionV2 pos = mapping.get((int) idx);
      positions.add(pos);
      idx = pos.parent - 1;
    }
    return positions;
  }

  private int findLineNumber(Long methodId, int line) {
    ArrayList<OffsetLine> lines = lineMappings.get(methodId);
    if (lines == null) {
      return -1;
    }
    OffsetLine bestLine = null;
    for (OffsetLine offsetLine : lines) {
      if (offsetLine.pc <= line) {
        bestLine = offsetLine;
      } else {
        if (bestLine == null) {
          // Better to give a rough line number than fail epicly.
          // Copied from Python symbolizer.
          bestLine = offsetLine;
        }
        break;
      }
    }
    if (bestLine != null) {
      return bestLine.mappedLine;
    }
    return -1;
  }

  public ArrayList<StackTraceElement> mapStackTrace(StackTraceElement[] trace) {
    ArrayList<StackTraceElement> newTrace = new ArrayList<>();
    for (StackTraceElement el : trace) {
      String fn = el.getFileName();
      if (fn.equals("")) {
        int line = el.getLineNumber();
        if (iodiMapping != null && lineMappings != null) {
          String name = el.getClassName() + "." + el.getMethodName();
          int layer = IODIConstants.getLayer(line);
          if (layer > 0) {
            name += "@" + layer;
            line = IODIConstants.getEncodedLine(line);
          }
          Long mappedId = iodiMapping.get(name);
          if (mappedId != null) {
            int mappedLine = findLineNumber(mappedId, line);
            if (mappedLine != -1) {
              line = mappedLine;
            }
          }
        }

        ArrayList<PositionV2> positions = getPositionsAt(line - 1);
        for (PositionV2 pos : positions) {
          newTrace.add(
              new StackTraceElement(
                  stringPool[pos.class_id],
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

  public PositionV2(int class_id, int method_id, int file_id, int line, long parent) {
    this.class_id = class_id;
    this.method_id = method_id;
    this.file_id = file_id;
    this.line = line;
    this.parent = parent;
  }
}
