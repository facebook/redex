/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package P;

import Q.D;

public class E extends D {
  public void simple_public_okay() {}

  public void package_private_collision3_not_okay() {}

  void package_private_collision3_okay() {}
}
