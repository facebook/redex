/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <unordered_set>

class DexType;

/**
 * This struct is populated by information from the config file.
 * "SynthPass" : {
 *   "max_passes" : <int>
 *   "synth_only" : <int>
 *   "remove_pub" : <int>
 *   "remove_constructors" : <int>
 * }
 * for the bool flags the value of the int is as expected (0 or non 0).
 * Meaning and default values for the flags:
 * - max number of passes to perform if there are possible wrappers
 * to remove that had to be dropped in a pass
 * max_passes = 5
 * - perform optimization only on synth methods
 * synth_only = 0 (false)
 * - allow removal of public methods
 * remove_pub = 1 (true)
 * - allow removal of synthetic constructors
 * remove_constructors = 1 (true)
 * those are the most "permissive" flags that optimize the highest number
 * of cases. No config definition is required if those are the flags used.
 */
struct SynthConfig {
  int64_t max_passes;
  bool synth_only;
  bool remove_pub;
  bool remove_constructors;
  std::unordered_set<const DexType*> blocklist_types;

  SynthConfig() {
    // defaults
    max_passes = 5;
    synth_only = false;
    remove_pub = true;
    remove_constructors = true;
  }
};
