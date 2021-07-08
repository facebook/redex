/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.fb.bundles;

import android.app.IntentService;
import android.content.Intent;

public class MyIntentService extends IntentService {

  public MyIntentService() {
    super("MyIntentService");
  }

  @Override
  protected void onHandleIntent(Intent intent) {}
}
