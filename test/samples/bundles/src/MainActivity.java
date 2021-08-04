/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.fb.bundles;

import com.facebook.R;

import android.app.Activity;
import android.os.Bundle;
import android.view.View;

public class MainActivity extends Activity {

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    setContentView(R.layout.activity_main);
  }

  public void performFoo(View v) {
    android.util.Log.w("BNDL", "performFoo!!");
  }

  public void performBar(View v) {
    android.util.Log.w("BNDL", "performBar!!");
  }
}
