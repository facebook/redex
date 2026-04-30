/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import static java.util.Locale.ENGLISH;
import static org.assertj.core.api.Assertions.assertThat;

import android.app.Activity;
import android.graphics.drawable.ColorDrawable;
import android.util.TypedValue;
import android.view.View;
import android.view.Window;
import android.widget.TextView;
import androidx.test.annotation.UiThreadTest;
import androidx.test.rule.ActivityTestRule;
import com.facebook.R;
import com.fb.styles.MainActivity;
import java.util.Locale;
import org.junit.Rule;
import org.junit.Test;

public class ResourceValueMergingPassResourcesTest {
  @Rule
  public ActivityTestRule<MainActivity> mActivityRule =
      new ActivityTestRule<>(MainActivity.class);

  @Test
  @UiThreadTest
  public void testUIValues() {
    MainActivity activity = mActivityRule.getActivity();

    TextView view1 = (TextView) activity.findViewById(R.id.textview1);
    assertTextColor(view1, "212121");
    assertBackgroundColor(view1, "FAFAFA");

    TextView view2 = (TextView) activity.findViewById(R.id.textview2);
    assertTextSize(view2, 16);
    assertTextColor(view2, "FFFFFF");
    assertBackgroundColor(view2, "2196F3");
    assertPadding(view2, 12);
  }

  static void assertTextColor(TextView view, String expected) {
    String actual = String.format(ENGLISH, "%06X", (0xFFFFFF & view.getCurrentTextColor()));
    assertThat(actual).as("Text color").isEqualTo(expected.toUpperCase(Locale.ENGLISH));
  }

  static void assertTextSize(TextView view, float expected) {
    float actual = view.getTextSize();
    assertThat(actual).as("Text size").isEqualTo(expected);
  }

  static void assertPadding(TextView view, float expected) {
    assertThat(view.getPaddingLeft()).as("Left padding").isEqualTo((int)expected);
    assertThat(view.getPaddingTop()).as("Top padding").isEqualTo((int)expected);
    assertThat(view.getPaddingRight()).as("Right padding").isEqualTo((int)expected);
    assertThat(view.getPaddingBottom()).as("Bottom padding").isEqualTo((int)expected);
  }

  static void assertBackgroundColor(View view, String expected) {
    int color = ((ColorDrawable) view.getBackground()).getColor();
    String actual = String.format(Locale.ENGLISH, "%06X", (0xFFFFFF & color));
    assertThat(actual).as("Background color").isEqualTo(expected.toUpperCase(Locale.ENGLISH));
    android.util.Log.w("BNDL", "Actual color: " + actual);
  }
}
