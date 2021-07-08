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

public class MainActivity extends Activity {
  @Override
  public void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);

    FBJNIExample f = new FBJNIExample(this);
    // BasicJNIExample b = new BasicJNIExample();  --- we probably don't need this, leave it out
    SimpleJNIExample s = new SimpleJNIExample(this);
    HybridJNIExample h = new HybridJNIExample();

    setTitle(R.string.app_name);
    setContentView(R.layout.hello);
    TextView textView = (TextView) findViewById(R.id.hello_text);
    try {
      int a = f.doThing() + s.doThing() + h.doThing();
      textView.setText("Hello " + a);
    } catch (Exception e) {
      textView.setText(String.format("Unable to load jni library! %s", e.getMessage()));
    }
  }
}
