/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import android.app.Activity;
import android.support.test.rule.ActivityTestRule;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;
import org.junit.Rule;
import org.junit.Test;

import com.fb.bundles.MainActivity;

public class BundleReachActivityTest {

  private static View findViewWithText(View parent, String toFind) {
    List<View> list = new ArrayList<>();
    list.add(parent);
    while (!list.isEmpty()) {
      View view = list.get(0);
      if (view instanceof ViewGroup) {
        ViewGroup viewGroup = (ViewGroup) view;
        int childCount = viewGroup.getChildCount();
        for (int i = 0; i < childCount; i++) {
          list.add(viewGroup.getChildAt(i));
        }
      }
      if (view instanceof TextView) {
        TextView textView = (TextView) view;
        CharSequence text = textView.getText();
        if (text != null && text.toString().contains(toFind)) {
          return view;
        }
      }
      list.remove(0);
    }
    throw new AssertionError("NOT FOUND: " + toFind);
  }

  @Rule
  public ActivityTestRule<MainActivity> mActivityRule =
    new ActivityTestRule<>(MainActivity.class);

  @Test
  public void testLaunchActivity() {
    // NOTE: Intentionally not using any good APIs for finding views/verifying,
    // just do something very rudimentary (in case things get obfuscated/changed
    // heavily).
    View decor = mActivityRule.getActivity().getWindow().getDecorView();
    {
      View result = findViewWithText(decor, "Oh my!");
      android.util.Log.w("BNDL", "FOUND: " + result.toString());
    }
    {
      View result = findViewWithText(decor, "Launch Theme Activity");
      android.util.Log.w("BNDL", "FOUND: " + result.toString());
    }
  }
}
