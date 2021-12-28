/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.fb.bundles;

import android.content.Context;
import android.content.res.TypedArray;
import android.util.AttributeSet;
import android.widget.Button;
import com.facebook.R;

public class WickedCoolButton extends Button {

  private boolean didAssert;

  public WickedCoolButton(Context context) {
    super(context);
  }

  public WickedCoolButton(Context context, AttributeSet attrs) {
    super(context, attrs);
    assertOnce(context, attrs);
  }

  public WickedCoolButton(Context context, AttributeSet attrs, int defStyleAttr) {
    super(context, attrs, defStyleAttr);
    assertOnce(context, attrs);
  }

  public WickedCoolButton(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
    super(context, attrs, defStyleAttr, defStyleRes);
    assertOnce(context, attrs);
  }

  private void assertOnce(Context context, AttributeSet attrs) {
    if (!didAssert) {
      TypedArray a = context.getTheme().obtainStyledAttributes(
          attrs,
          R.styleable.WickedCoolButton,
          0,
          0);
      try {
        boolean b = a.getBoolean(R.styleable.WickedCoolButton_a_boolean, false);
        if (!b) {
          throw new AssertionError("Unexpected value: " + b);
        }
        int flags = a.getInteger(R.styleable.WickedCoolButton_fancy_effects, 0);
        if (flags != (0x8 | 0x2)) {
          throw new AssertionError("Unexpected value: 0x" + Integer.toHexString(flags));
        }
        int type = a.getInteger(R.styleable.WickedCoolButton_reverb_type, 0);
        if (type != 1) {
          throw new AssertionError("Unexpected value: " + type);
        }
        android.util.Log.w("BNDL", "WickedCoolButton inflated with correct values " + b + ", 0x" + Integer.toHexString(flags) + ", " + typeToName(type));
      } finally {
        a.recycle();
      }
      didAssert = true;
    }
  }

  private static String typeToName(int type) {
    switch (type) {
      case 0:
        return "hall";
      case 1:
        return "spring";
      case 2:
        return "plate";
      case 3:
        return "shimmer";
      default:
        throw new IllegalArgumentException(String.valueOf(type));
    }
  }
}
