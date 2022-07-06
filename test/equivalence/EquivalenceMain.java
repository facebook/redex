/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.equivalence;
import java.lang.reflect.*;
import java.util.*;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;
import android.test.InstrumentationTestCase;
import org.junit.Test;
import org.junit.Assert;

public class EquivalenceMain extends InstrumentationTestCase {

  // XXX: it would be cool if the C++ test_generator generated a @Test-annotated
  // method for each before-after pair that would do the assertEquals() call.
  // That would allow us to reuse the JUnit code instead of creating our own
  // ad-hoc test-runner-within-a-test-runner here. I just didn't want to write
  // too much bytecode by hand.
  @Test
  public void testAll() throws Exception {
    Class c = this.getClass();
    HashMap<String, Method> beforeMethods = new HashMap<String, Method>();
    HashMap<String, Method> afterMethods = new HashMap<String, Method>();
    for (Method m : c.getDeclaredMethods()) {
      String name = m.getName();
      if (name.startsWith("before_")) {
        String suffix = name.replaceFirst("before_", "");
        beforeMethods.put(suffix, m);
      } else if (name.startsWith("after_")) {
        String suffix = name.replaceFirst("after_", "");
        afterMethods.put(suffix, m);
      }
    }
    writeToAdb("\n");
    for (Map.Entry<String, Method> entry : beforeMethods.entrySet()) {
      Bundle bundle = new Bundle();
      writeToAdb(String.format("Running %s\n", entry.getKey()));
      Method before = entry.getValue();
      Method after = afterMethods.get(entry.getKey());
      Assert.assertEquals(before.invoke(this), after.invoke(this));
    }
  }

  private void writeToAdb(String s) {
    Instrumentation instr = getInstrumentation();
    Bundle bundle = new Bundle();
    bundle.putString(Instrumentation.REPORT_KEY_STREAMRESULT, s);
    instr.sendStatus(Activity.RESULT_OK, bundle);
  }

}
