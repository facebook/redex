/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <json/json.h>

enum Architecture {
  UNKNOWN,
  ARM,
  ARMV7,
  ARM64,
  X86_64,
  X86,
};

class RedexOptions {
 public:
  bool verify_none_enabled{false};
  bool is_art_build{false};
  bool instrument_pass_enabled{false};
  int32_t min_sdk{0};
  Architecture arch{Architecture::UNKNOWN};

  /*
   * Overwriting the `this` register breaks the verifier before Android M and
   * the Java debugger before Android Q.
   *
   * https://r8.googlesource.com/r8/+/heads/d8-1.4/src/main/java/com/android/tools/r8/utils/InternalOptions.java#701
   */
  bool no_overwrite_this() const { return min_sdk < 29; }

  // Encode the struct to entry_data for redex-opt tool.
  void serialize(Json::Value& entry_data) const;

  // Decode the entry_data and update the struct.
  void deserialize(const Json::Value& entry_data);
};

Architecture parse_architecture(const std::string& s);

const char* architecture_to_string(Architecture arch);
