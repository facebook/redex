/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.fb.resources;

import android.content.res.Resources;
import com.facebook.R;

import android.app.Activity;
import android.os.Bundle;

public class MainActivity extends Activity {

  private static final String LOG_TAG = "REDEX";

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    setContentView(R.layout.activity_main);
    logValues();
  }

  private void logValues() {
    Resources resources = getResources();
    if (resources.getBoolean(R.bool.should_log)) {
      int simple = resources.getColor(R.color.simple);
      android.util.Log.w(LOG_TAG, "simple: " + Integer.toHexString(simple));

      int txt = resources.getColor(R.color.text_color);
      android.util.Log.w(LOG_TAG, "text_color: " + Integer.toHexString(txt));

      int bg = resources.getColor(R.color.view_bg);
      android.util.Log.w(LOG_TAG, "view_bg: " + Integer.toHexString(bg));

      int count = resources.getInteger(R.integer.loop_count);
      String main = resources.getString(R.string.main_text);
      for (int i = 0; i < count; i++) {
        android.util.Log.w(LOG_TAG, "main_text: " + main + " [" + (i + 1) + " / " + count + "]");
      }

      int size = resources.getDimensionPixelSize(R.dimen.text_size);
      android.util.Log.w(LOG_TAG, "Text size (in pixels): " + size);
    }
  }
}
