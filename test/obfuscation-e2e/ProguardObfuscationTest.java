/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
