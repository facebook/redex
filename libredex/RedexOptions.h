/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string>

namespace Json {
class Value;
} // namespace Json

enum Architecture {
  UNKNOWN,
  ARM,
  ARMV7,
  ARM64,
  X86_64,
  X86,
};

enum class DebugInfoKind : uint32_t {
  // This turns off all symbol files aside from the ProGuard-style symbol map.
  NoCustomSymbolication,
  // This (and all options further below) will turn on emission of the line
  // number map and the {class,method}_mapping.txt.
  PerMethodDebug,
  // This will cause us not to emit a debug_info_item for any method.
  NoPositions,
  // This will cause us to emit just a few debug_info_items per dex, one for
  // each method parameter arity.
  InstructionOffsets,
  // This will cause us to emit just a few debug_info_items per dex, one for
  // each method parameter arity. Overloaded methods are handled via layering.
  InstructionOffsetsLayered,
  // This will cause us to emit debug info for each instruction as well as
  // debug info for tracking registers as local variables.
  BytecodeDebugger,
  Size,
};

class RedexOptions {
 public:
  bool verify_none_enabled{false};
  bool is_art_build{false};
  bool disable_dex_hasher{false};
  bool instrument_pass_enabled{false};
  bool post_lowering{false};
  int32_t min_sdk{0};
  Architecture arch{Architecture::UNKNOWN};
  DebugInfoKind debug_info_kind{DebugInfoKind::NoCustomSymbolication};
  std::string jni_summary_path;

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

DebugInfoKind parse_debug_info_kind(const std::string&);

std::string debug_info_kind_to_string(const DebugInfoKind& kind);

bool is_iodi(const DebugInfoKind&);
