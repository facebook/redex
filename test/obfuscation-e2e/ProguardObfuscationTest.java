/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.test.proguard;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public class ProguardObfuscationTest extends Activity {
  @Override
  public void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    TextView text = new TextView(this);
    text.setText("Redex Proguard Obfuscation Tests");
    setContentView(text);
    Alpha a = new Alpha();
    text.append("doubleWombat = " + a.doubleWombat());
  }
}
