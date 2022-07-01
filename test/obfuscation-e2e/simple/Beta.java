/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.proguard;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public class Beta extends Activity {

    private int wombatBeta;
    /*public Beta () {
        wombatBeta = 72;
    }*/

    @Override
    public void onCreate(Bundle savedInstanceState) {
      super.onCreate(savedInstanceState);
      TextView text = new TextView(this);
      wombatBeta = 5;

      All all = new All();
      text.setText(all.all());

      setContentView(text);
    }

    private int privateDmethod() { return 10; }

    public int doubleWombatBeta() {
        privateDmethod();
        return 2 * wombatBeta;
    }
}

class Hello {
  public String hello = "hello";

  public Hello() {}
  public Hello(int x) {}

  public String get() {
    return hello;
  }
}

class World extends Hello {
  public String world = "world";

  public String get() {
    return world;
  }
}

class All extends World {
  public String all() {
    return hello + " " + world;
  }
}

/*class Together {
  public String hello = "hello";
  public static int world = 1;
}*/
