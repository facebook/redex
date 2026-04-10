/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

// Simulates an untyped data source like android.os.Parcel — returns raw values
// without typedef annotations. Used to test that do_not_check_list skips these.
public class UntypedSource {
    public static int readInt() { return 0; }
    public static String readString() { return ""; }
}
