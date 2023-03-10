/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import static org.assertj.core.api.Assertions.assertThat;

import android.app.Activity;
import android.content.res.Resources;
import androidx.test.rule.ActivityTestRule;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import com.facebook.R;
import com.fb.bundles.MainActivity;
import java.util.ArrayList;
import java.util.List;
import org.junit.Rule;
import org.junit.Test;

public class DedupResourceActivityTest {

  @Rule
  public ActivityTestRule<MainActivity> mActivityRule =
      new ActivityTestRule<>(MainActivity.class);

  @Test
  public void testLaunchActivity() {
    Resources resource = mActivityRule.getActivity().getResources();
    float dp = resource.getDisplayMetrics().density;
    float sp = resource.getDisplayMetrics().scaledDensity;
    assertThat(resource.getDimension(R.dimen.unused_dimen_1) / sp).isEqualTo(8);
    assertThat(resource.getDimension(R.dimen.unused_dimen_2) / dp)
        .isEqualTo(16);
    assertThat(resource.getDimension(R.dimen.margin_top) / dp).isEqualTo(24);
    assertThat(resource.getDimension(R.dimen.padding_left) / dp).isEqualTo(16);
    assertThat(resource.getDimension(R.dimen.padding_right) / dp).isEqualTo(16);
    assertThat(resource.getDimension(R.dimen.welcome_text_size) / sp)
        .isEqualTo(18);
    assertThat(resource.getDimension(R.dimen.small) / dp).isEqualTo(4);
    assertThat(resource.getDimension(R.dimen.medium) / dp).isEqualTo(8);
    assertThat(resource.getDimension(R.dimen.medium2) / dp).isEqualTo(8);
    assertThat(resource.getDimension(R.dimen.baz) / dp).isEqualTo(8);
    assertThat(resource.getDimension(R.dimen.boo) / dp).isEqualTo(8);
    assertThat(resource.getDimension(R.dimen.far) / dp).isEqualTo(4);
  }
}
