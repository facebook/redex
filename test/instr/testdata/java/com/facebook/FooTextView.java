/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook;

import android.content.Context;
import android.util.AttributeSet;
import android.widget.TextView;

public class FooTextView extends TextView {
    public FooTextView(Context context) {
        super(context);
    }

    public FooTextView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    public FooTextView(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
    }

    public FooTextView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(context, attrs, defStyleAttr, defStyleRes);
    }
}
