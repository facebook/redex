/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.counter;

import android.app.Activity;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

public class Counter extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout layout = new LinearLayout(this);

        final TextView countText = new TextView(this);
        countText.setText("0");

        Button up = new Button(this);
        up.setText("Up");
        up.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Integer c = Integer.parseInt(countText.getText().toString());
                c++;
                countText.setText(c.toString());
            }
        });
        layout.addView(up);

        Button down = new Button(this);
        down.setText("Down");
        down.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Integer c = Integer.parseInt(countText.getText().toString());
                c--;
                countText.setText(c.toString());
            }
        });
        layout.addView(down);

        layout.addView(countText);

        setContentView(layout);
    }
}
