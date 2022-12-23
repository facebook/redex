/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import androidx.test.rule.ActivityTestRule;
import org.junit.Rule;
import org.junit.Test;

import com.facebook.resourcetest.SimpleActivity;

public class LaunchSimpleActivityTest {

  @Rule
  public ActivityTestRule<SimpleActivity> mActivityRule =
    new ActivityTestRule<>(SimpleActivity.class);

  @Test
  public void testLaunchActivity() {
    // Intentionally left blank. A @Test method and the @Rule will just launch
    // the default Activity, which crashes if the test should fail.
  }
}
