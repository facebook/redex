/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.resourcetest;

import android.content.res.Resources;

public class ResourcesCompat {

  public final static int getIdentifier(
    Resources r,
    String name,
    String defType,
    String defPackage) {
    android.util.Log.w("ResourcesCompat", "[Lookup] " + name);
    int result = r.getIdentifier(name, defType, defPackage);
    if (result != 0) {
      return result;
    }
    android.util.Log.w("ResourcesCompat", "[Fallback] " + name);
    result = r.getIdentifier(name, defType + ".2", defPackage);
    if (result != 0) {
      return result;
    }
    android.util.Log.w("ResourcesCompat", "[Fallback Again] " + name);
    return r.getIdentifier(name, defType + ".3", defPackage);
  }

  public final static String getResourceTypeName(Resources r, int id) {
    android.util.Log.w(
      "ResourcesCompat",
      "[Lookup] 0x" + Integer.toString(id, 16));
    String name = r.getResourceTypeName(id);
    if (name == null) {
      return name;
    }
    int len = name.length();
    if (len < 3 || !Character.isDigit(name.charAt(len - 1)) ) {
      return name;
    }
    android.util.Log.w("ResourcesCompat", "[Fallback] " + name);
    return name.substring(0, len - 2);
  }
}
