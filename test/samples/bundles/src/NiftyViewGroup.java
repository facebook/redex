/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.fb.bundles;

import android.content.Context;
import android.util.AttributeSet;
import android.widget.LinearLayout;

public class NiftyViewGroup extends LinearLayout {
  public NiftyViewGroup(Context context) {
    super(context);
  }

  public NiftyViewGroup(Context context, AttributeSet attrs) {
    super(context, attrs);
  }

  public NiftyViewGroup(Context context, AttributeSet attrs, int defStyleAttr) {
    super(context, attrs, defStyleAttr);
  }

  public NiftyViewGroup(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
    super(context, attrs, defStyleAttr, defStyleRes);
  }
}
