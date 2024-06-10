/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package Q;

import P.E;

public class F extends E {
  public void simple_public_okay() {}

  void package_private_collision3_okay() {}

  public void package_private_collision3_not_okay() {}

  public void interface_collision_not_okay() {}
}
