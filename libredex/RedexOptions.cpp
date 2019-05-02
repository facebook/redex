/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RedexOptions.h"

void RedexOptions::serialize(Json::Value& entry_data) const {
  auto& options = entry_data["redex_options"];
  options["verify_none_enabled"] = verify_none_enabled;
  options["is_art_build"] = is_art_build;
  options["instrument_pass_enabled"] = instrument_pass_enabled;
  options["min_sdk"] = min_sdk;
}

void RedexOptions::deserialize(const Json::Value& entry_data) {
  const auto& options_data = entry_data["redex_options"];
  verify_none_enabled = options_data["verify_none_enabled"].asBool();
  is_art_build = options_data["is_art_build"].asBool();
  instrument_pass_enabled = options_data["instrument_pass_enabled"].asBool();
  min_sdk = options_data["min_sdk"].asInt();
}

Architecture parse_architecture(const std::string& s) {
  if (s == "arm")
    return Architecture::ARM;
  else if (s == "armv7")
    return Architecture::ARMV7;
  else if (s == "arm64")
    return Architecture::ARM64;
  else if (s == "x86_64")
    return Architecture::X86_64;
  else if (s == "x86")
    return Architecture::X86;
  else
    return Architecture::UNKNOWN;
}

const char* architecture_to_string(Architecture arch) {
  switch (arch) {
  case Architecture::ARM:
    return "arm";
  case Architecture::ARMV7:
    return "armv7";
  case Architecture::ARM64:
    return "arm64";
  case Architecture::X86_64:
    return "x86_64";
  case Architecture::X86:
    return "x86";
  default:
    return "UNKNOWN";
  }
}
