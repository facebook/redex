/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex.jni.example;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

import com.facebook.soloader.DoNotOptimize;
import com.facebook.soloader.SoLoader;
import com.facebook.soloader.annotation.SoLoaderLibrary;

@SoLoaderLibrary("Animal")
@DoNotOptimize
public class Main extends Activity {
  public static native String implementedButUnused(int value);
  public static native String implemented(String name, int value);
  public static native String missing(String name, int value);
  @Override
  public void onCreate(Bundle savedInstanceState) {
    SoLoader.init(this, false);
    SoLoader.loadLibrary("Animal");
    super.onCreate(savedInstanceState);

    setTitle(R.string.app_name);
    setContentView(R.layout.hello);
    TextView textView = (TextView) findViewById(R.id.hello_text);
    try {
      String s = implemented("Hello", 99);
      textView.setText(s);
    } catch (Exception e) {
      textView.setText(String.format(
              "Unable to load jni library! %s",
              e.getMessage()));
    }
  }
}
