/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.fb.bundles;

import com.facebook.R;

import android.content.Context;
import android.widget.Toast;

public class Unused {
  public static void showText(Context context, String txt) {
    Toast.makeText(context, context.getString(R.string.toast_fmt, txt), Toast.LENGTH_SHORT).show();
  }
}
