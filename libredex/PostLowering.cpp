/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PostLowering.h"
#include "DexOutput.h"

class NoopPostLowering : public PostLowering {
 public:
  void sync() override {}
  void run(const DexStoresVector& stores) override {}
  void finalize(AssetManager& mgr) override {}

  std::unordered_map<DexClass*, std::vector<DexMethod*>> get_detached_methods()
      override {
    return std::unordered_map<DexClass*, std::vector<DexMethod*>>();
  }
  void emit_symbolication_metadata(
      PositionMapper* pos_mapper,
      std::unordered_map<DexMethod*, uint64_t>* method_to_id,
      std::unordered_map<DexCode*, std::vector<DebugLineItem>>*
          code_debug_lines,
      IODIMetadata* iodi_metadata,
      std::vector<DexMethod*>& needs_debug_line_mapping,
      std::set<uint32_t>& signatures) override {}

  void load_dex_indexes(ConfigFiles&,
                        int32_t,
                        DexClasses*,
                        GatheredTypes&,
                        const std::string&,
                        size_t) override {}
};

std::unique_ptr<PostLowering> PostLowering::create() {
  return std::make_unique<NoopPostLowering>();
}
