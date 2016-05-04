/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

/**
 * This struct is populated by information from the config file.
 * "SynthPass" : {
 *   "max_passes" : <int>
 *   "synth_only" : <int>
 *   "remove_pub" : <int>
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
 * those are the most "permissive" flags that optimize the highest number
 * of cases. No config definition is required if those are the flags used.
 */
struct SynthConfig {
  int64_t max_passes;
  bool synth_only;
  bool remove_pub;
};
