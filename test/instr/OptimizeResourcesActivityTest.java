/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import android.app.Activity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import androidx.test.rule.ActivityTestRule;
import androidx.test.annotation.UiThreadTest;
import java.util.ArrayList;
import java.util.List;
import com.fb.bundles.MainActivity;
import org.junit.Rule;
import org.junit.Test;

public class OptimizeResourcesActivityTest {

  @Rule
  public ActivityTestRule<MainActivity> mActivityRule =
      new ActivityTestRule<>(MainActivity.class);

  private static void findAndPressButton(View parent) {
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
      if (view instanceof Button) {
        view.performClick();
        return;
      }
      list.remove(0);
    }
    throw new AssertionError("NO BUTTON TO PRESS");
  }

  @Test
  @UiThreadTest
  public void testLaunchActivity() {
    // Activity will crash itself if things are wrong and fail the test
    findAndPressButton(mActivityRule.getActivity().getWindow().getDecorView());
  }

  static class UsedResources {
    public static final int FOO = 0x7f040008;

    public void bar() {
      int a = 0x7f040000;
      int b = 0x7f090000;
      android.util.Log.w("UsedResources",
                         "USING " + a + " " + b +
                             "/2130903043 2130837504 21308375050000 " + FOO);
    }
  }
}
