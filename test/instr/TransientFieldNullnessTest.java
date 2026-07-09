/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.instr;

import static org.assertj.core.api.Assertions.assertThat;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.ObjectInputStream;
import java.io.ObjectOutputStream;
import java.io.Serializable;
import org.junit.Test;

/**
 * Regression test for an IPCP soundness bug. {@code Job.buffer} is {@code transient}, so after Java
 * deserialization it is null ({@code ObjectInputStream} skips {@code <init>}). {@code run} null-checks
 * {@code buffer} before calling a method on it; the bug is that IPCP proves the field non-null from
 * its {@code <init>} store and deletes the null-check, so the call dereferences null.
 *
 * <p>The ProGuard config includes the production {@code Serializable} keep rule, which keeps
 * non-transient fields but not transient ones, so the crash reproduces under production semantics.
 */
public class TransientFieldNullnessTest {

  static final class Job implements Serializable {
    private transient StringBuilder buffer = new StringBuilder();

    // The `if (buffer == null)` guard must survive IPCP: a `buffer` load followed by a branch on it.
    // CHECK: method: {{(direct|virtual)}} {{.*}}TransientFieldNullnessTest$Job.run:()int
    // CHECK: iget-object [[B:v[0-9]+]], {{v[0-9]+}}, {{.*}}buffer:
    // CHECK-NEXT: if-{{(eq|ne)}}z [[B]]
    int run() {
      if (buffer == null) {
        buffer = new StringBuilder();
      }
      return buffer.length();
    }
  }

  private static Job roundTripThroughSerialization(Job job) throws Exception {
    ByteArrayOutputStream bytes = new ByteArrayOutputStream();
    try (ObjectOutputStream out = new ObjectOutputStream(bytes)) {
      out.writeObject(job);
    }
    try (ObjectInputStream in =
        new ObjectInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
      return (Job) in.readObject();
    }
  }

  @Test
  public void deserializedJobHonorsNullGuard() throws Exception {
    Job job = roundTripThroughSerialization(new Job());
    // buffer is null after deserialization; run() must re-create it via the null guard (length 0)
    // instead of dereferencing null.
    assertThat(job.run()).isEqualTo(0);
  }
}
