/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

class DexMethod;

namespace bridge_synth_inline_internal {

bool rewrite_bridge_with_abstract_super_target(DexMethod* method);

} // namespace bridge_synth_inline_internal
