/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.examples.proguardexample;

import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.widget.TextView;

public class MainActivity extends AppCompatActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        TextView textView = (TextView) findViewById(R.id.message);

        Alpha alphaObject = new Alpha();
        int ltuae = alphaObject.doubleWombat();
        textView.setText("The answer is " + ltuae + "\n");

        try {
            Class<?> greek = Class.forName("com.facebook.redex.examples.proguardexample.Greek");
            if (greek.isInstance(alphaObject)) {
                textView.append("Alpha is an instance of Greek");
            } else {
                textView.append("Alpha is not an instance of Greek");
            }
        } catch (ClassNotFoundException e) {
            textView.append("ERROR: Greek interface not found");
        }
    }
}
