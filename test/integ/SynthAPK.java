/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redextest;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public class SynthAPK extends Activity {
  @Override
  public void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    TextView text = new TextView(this);

    Alpha a = new Alpha(12);
    Alpha.Beta b = a.new Beta();
    String s = "";

    try {
      java.io.Writer writer = new java.io.StringWriter();
      writer.write("hello");
      Gamma g = new Gamma(writer);
      Gamma.Delta d = g.new Delta();
      s = d.getWriter().toString();
    } catch (java.io.IOException e) {
      s = e.toString();
    }

    text.setText("Static Synth 2*12 = " + b.doublex() + " [" + s + "]\n");
    setContentView(text);
  }
}
