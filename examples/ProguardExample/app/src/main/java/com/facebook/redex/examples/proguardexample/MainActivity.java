/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
