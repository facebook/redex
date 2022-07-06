/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.fb.strings;

import android.app.Activity;
import android.content.res.Resources;
import android.os.Bundle;

import com.facebook.R;

public class MainActivity extends Activity {

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    Resources resources = getResources();
    android.util.Log.w("STR", resources.getString(R.string.one).toString());
    android.util.Log.w("STR", resources.getString(R.string.two).toString());
  }
}
