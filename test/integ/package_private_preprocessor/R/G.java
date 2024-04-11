/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package R;

import Q.F;
import R.I;

public class G extends F implements I {
  public void simple_public_okay() {}

  void package_private_collision3_okay() {}
}
