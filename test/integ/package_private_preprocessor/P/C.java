/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package P;

public class C {
  public void simple_public_okay() {}

  void package_private_collision1_okay() {}

  void package_private_collision2_okay() {}

  void package_private_collision3_okay() {}

  void package_private_collision3_not_okay() {}

  void interface_collision_not_okay() {}
}
