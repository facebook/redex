/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.fb.bundles;

import static java.util.Locale.ENGLISH;

import android.content.Intent;
import android.content.res.Resources;
import android.widget.TextView;
import com.facebook.R;

import android.app.Activity;
import android.os.Bundle;
import android.view.View;

public class MainActivity extends Activity {

  private static final String[] EXPECTED_STRINGS = {
    "Fig",
    "Cara cara navel",
    "Mango"
  };

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    setContentView(R.layout.activity_main);
    TextView view = (TextView) findViewById(R.id.welcome_view);
    String textColor = String.format(ENGLISH, "%06X", (0xFFFFFF & view.getCurrentTextColor()));
    if (!"8A9E46".equals(textColor)) {
      throw new AssertionError("Unexpected value: " + textColor);
    }
    Resources resources = getResources();
    String[] fruits = resources.getStringArray(R.array.some_fruits);
    if (fruits == null || fruits.length != EXPECTED_STRINGS.length) {
      throw new AssertionError("Not enough fruit");
    }
    for (int i = 0; i < fruits.length; i++) {
      String fruit = fruits[i];
      android.util.Log.w("BNDL", fruit);
      if (!fruit.equals(EXPECTED_STRINGS[i])) {
        throw new AssertionError("Wrong value: " + fruit);
      }
    }
    String sentence = resources.getQuantityString(R.plurals.a_sentence_with_geese, fruits.length);
    if (!sentence.contains("geese")) {
      throw new AssertionError("Unexpected value: " + sentence);
    }
    android.util.Log.w("BNDL", sentence);
  }

  public void performFoo(View v) {
    android.util.Log.w("BNDL", "performFoo!!");
    startActivity(new Intent(this, com.fb.bundles.ThemeActivity.class));
  }

  public void performBar(View v) {
    android.util.Log.w("BNDL", "performBar!!");
  }
}
