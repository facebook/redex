/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

import android.app.Fragment;
import android.content.Context;
import android.os.Bundle;
import android.view.ContextThemeWrapper;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import com.facebook.R;

public class B extends Fragment {

  @Override
  public View onCreateView(
      LayoutInflater inflater,
      ViewGroup container,
      Bundle savedInstanceState) {
    ContextThemeWrapper wrapped =
        new ContextThemeWrapper(inflater.getContext(), R.style.CustomText_Prickly);
    LayoutInflater effectiveInflater =
        (LayoutInflater) wrapped.getSystemService(Context.LAYOUT_INFLATER_SERVICE);
    TextView root =
        (TextView) effectiveInflater.inflate(android.R.layout.simple_list_item_1, container, false);
    root.setText("Enjoy!!!!");
    return root;
  }
}
