/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package Q;

import P.C;

public class D extends C {

  void package_private_collision1_okay() {}

  public void package_private_collision2_okay() {}

  void package_private_collision3_not_okay() {}

  void package_private_collision3_okay() {}
}
