/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.fb.bundles;

import android.content.Context;
import android.graphics.drawable.ColorDrawable;
import android.view.ContextThemeWrapper;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import com.facebook.R;

import android.app.Activity;
import android.os.Bundle;
import java.util.Locale;

public class ThemeActivity extends Activity {

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    LinearLayout layout = new LinearLayout(this);
    layout.setOrientation(LinearLayout.VERTICAL);
    setContentView(layout);
    // Should have a purple background, as specified in theme from AndroidManifest.xml.
    appendView(this, layout);
    ContextThemeWrapper themeWrapper = new ContextThemeWrapper(this, R.style.ThemeB);
    // Should have a pink-ish background, as specified in ThemeB.
    appendView(themeWrapper, layout);
    // Validate
    assertBackgroundColor(layout.getChildAt(0), "9C27B0");
    assertBackgroundColor(layout.getChildAt(1), "E91E63");
  }

  static void appendView(Context context, ViewGroup parent) {
    LayoutInflater inflater = (LayoutInflater) context.getSystemService(LAYOUT_INFLATER_SERVICE);
    inflater.inflate(R.layout.themed, parent, true);
  }

  static void assertBackgroundColor(View view, String expected) {
    int color = ((ColorDrawable) view.getBackground()).getColor();
    String actual = String.format(Locale.ENGLISH, "%06X", (0xFFFFFF & color));
    if (!actual.equals(expected.toUpperCase(Locale.ENGLISH))) {
      throw new AssertionError("Unexpected background: " + actual);
    }
    android.util.Log.w("BNDL", "Actual color: " + actual);
  }
}
