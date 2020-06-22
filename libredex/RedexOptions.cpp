/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RedexOptions.h"

#include "Debug.h"

void RedexOptions::serialize(Json::Value& entry_data) const {
  auto& options = entry_data["redex_options"];
  options["verify_none_enabled"] = verify_none_enabled;
  options["is_art_build"] = is_art_build;
  options["enable_pgi"] = enable_pgi;
  options["disable_dex_hasher"] = disable_dex_hasher;
  options["instrument_pass_enabled"] = instrument_pass_enabled;
  options["min_sdk"] = min_sdk;
  options["debug_info_kind"] = debug_info_kind_to_string(debug_info_kind);
  options["redacted"] = redacted;
}

void RedexOptions::deserialize(const Json::Value& entry_data) {
  const auto& options_data = entry_data["redex_options"];
  verify_none_enabled = options_data["verify_none_enabled"].asBool();
  is_art_build = options_data["is_art_build"].asBool();
  enable_pgi = options_data["enable_pgi"].asBool();
  disable_dex_hasher = options_data["disable_dex_hasher"].asBool();
  instrument_pass_enabled = options_data["instrument_pass_enabled"].asBool();
  min_sdk = options_data["min_sdk"].asInt();
  debug_info_kind =
      parse_debug_info_kind(options_data["debug_info_kind"].asString());
  redacted = options_data["redacted"].asBool();
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

DebugInfoKind parse_debug_info_kind(const std::string& raw_kind) {
  if (raw_kind == "no_custom_symbolication" || raw_kind.empty()) {
    return DebugInfoKind::NoCustomSymbolication;
  } else if (raw_kind == "per_method_debug") {
    return DebugInfoKind::PerMethodDebug;
  } else if (raw_kind == "no_positions") {
    return DebugInfoKind::NoPositions;
  } else if (raw_kind == "iodi") {
    return DebugInfoKind::InstructionOffsets;
  } else if (raw_kind == "bytecode_debugger") {
    return DebugInfoKind::BytecodeDebugger;
  } else {
    std::ostringstream os;
    bool first{true};
    for (uint32_t i = 0; i < static_cast<uint32_t>(DebugInfoKind::Size); ++i) {
      if (!first) {
        os << ", ";
      }
      first = false;
      os << '"' << debug_info_kind_to_string(static_cast<DebugInfoKind>(i))
         << '"';
    }
    not_reached_log("Unknown debug info kind. Supported kinds are %s",
                    os.str().c_str());
  }
}

std::string debug_info_kind_to_string(const DebugInfoKind& kind) {
  switch (kind) {
  case DebugInfoKind::NoCustomSymbolication:
    return "no_custom_symbolication";
  case DebugInfoKind::PerMethodDebug:
    return "per_method_debug";
  case DebugInfoKind::NoPositions:
    return "no_positions";
  case DebugInfoKind::InstructionOffsets:
    return "iodi";
  case DebugInfoKind::BytecodeDebugger:
    return "bytecode_debugger";
  case DebugInfoKind::Size:
    not_reached_log("DebugInfoKind::Size should not be used");
  }
}

bool is_iodi(const DebugInfoKind& kind) {
  return kind == DebugInfoKind::InstructionOffsets;
}
