/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.resourcetest;

import android.app.Activity;
import android.content.res.Resources;
import android.graphics.drawable.ColorDrawable;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.util.Log;
import android.widget.TextView;

import com.facebook.R;

public class SimpleActivity extends Activity {

  private static final String TAG = "SimpleActivity";

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    setContentView(R.layout.simple_layout);

    Resources resources = getResources();
    DisplayMetrics metrics = resources.getDisplayMetrics();

    Log.i(TAG, "NOTE: current device density: " + metrics.density);

    TextView root = (TextView) findViewById(R.id.verify_target);
    if (root == null) {
      throw new RuntimeException("Root view not found");
    }
    int sizeDp = (int) (root.getTextSize() / metrics.density);
    if (sizeDp != 36) {
      throw new RuntimeException("text size incorrect");
    }
    int color = ((ColorDrawable) root.getBackground()).getColor();
    if (color != 0xffd75db6) {
      throw new RuntimeException("bg color incorrect");
    }

    assertDpValue(R.dimen.test_dimen_ref, resources, metrics, 50, "test_dimen_ref");
    assertDpValue(R.dimen.sample_dp_90, resources, metrics, 90, "sample_dp_90");

    assertDpValue(resources, "dimen", "sample_dp_32", 32);
    assertDpValue(resources, "dimen", "sample_dp_64", 64);

    validatePlural(resources, R.plurals.simple_plural_str, 1, "1 banana");
    validatePlural(resources, R.plurals.simple_plural_str, 100, "Wow! 100 fabulous bananas!");

    validatePlural(resources, R.plurals.simple_plural_str2, 1, "1 kerfuffle");
    validatePlural(resources, R.plurals.simple_plural_str2, 2, "2 kerfuffles");

    validatePlural(resources, R.plurals.multi_plural_str, 1, "1 apple");
    validatePlural(resources, R.plurals.multi_plural_str, 1000, "1000 apples");

    assertTypeName(resources, R.dimen.sample_dp_90, "dimen");
  }

  private static void assertDpValue(
    int identifier,
    Resources resources,
    DisplayMetrics metrics,
    int expectedValue,
    String debugMsg) {
    Log.i(TAG, "Verifying dimen " + debugMsg);
    int px = resources.getDimensionPixelSize(identifier);
    int dp = Math.round(px / metrics.density);
    if (dp != expectedValue) {
      throw new RuntimeException(debugMsg + " has incorrect value. Got: " + dp);
    }
  }

  private void assertDpValue(Resources resources, String type, String key, int expectedValue) {
    int id = resources.getIdentifier(key, type, getPackageName());
    if (id == 0) {
      throw new RuntimeException("Resource " + type + "/" + key + " not found");
    }
    assertDpValue(
      id,
      resources,
      resources.getDisplayMetrics(),
      expectedValue,
      key);
  }

  private static void validatePlural(
    Resources resources,
    int id,
    int quantity,
    String expected) {
      String actual = resources.getQuantityString(id, quantity, quantity);
    String debugMsg =
      "Plural lookup: 0x" +
      Integer.toString(id, 16) +
      (quantity == 1 ? " (singular)" : " (many)") +
      " -> " +
      actual;
    Log.i(TAG, debugMsg);
    if (!expected.equals(actual)) {
      throw new RuntimeException("Incorrect plural value, expected: " + expected);
    }
  }

  private static void assertTypeName(
    Resources resources,
    int id,
    String expectedType) {
    String actual = resources.getResourceTypeName(id);
    if (!expectedType.equals(actual)) {
      String debugMsg = "Incorrect resource type for identifier: 0x" +
        Integer.toString(id, 16) +
        ". Got: " + actual;
      throw new RuntimeException(debugMsg);
    }
  }
}
