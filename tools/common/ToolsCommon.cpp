/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "ToolsCommon.h"

#include "DexOutput.h"
#include "DexUtil.h"
#include "IRMetaIO.h"
#include "InstructionLowering.h"
#include "Timer.h"

/**
 * Write meta data to file.
 * Development usage only
 */
void write_ir_meta(const std::string& output_ir_dir, DexStoresVector& stores) {
  Timer t("Dummping IR meta");
  Scope classes = build_class_scope(stores);
  ir_meta_io::dump(classes, output_ir_dir);
}

/**
 * Write intermediate dex to files.
 * Development usage only
 */
void write_intermediate_dex(const ConfigFiles& cfg,
                            const std::string& output_ir_dir,
                            DexStoresVector& stores) {
  {
    Timer t("Instruction lowering");
    instruction_lowering::run(stores);
  }
  std::unique_ptr<PositionMapper> pos_mapper(PositionMapper::make("", ""));
  for (auto& store : stores) {
    Timer t("Writing optimized dexes");
    for (size_t i = 0; i < store.get_dexen().size(); i++) {
      std::ostringstream ss;
      ss << output_ir_dir << "/" << store.get_name();
      if (store.get_name().compare("classes") == 0) {
        // primary/secondary dex store, primary has no numeral and secondaries
        // start at 2
        if (i > 0) {
          ss << (i + 1);
        }
      } else {
        // other dex stores do not have a primary,
        // so it makes sense to start at 2
        ss << (i + 2);
      }
      ss << ".dex";
      write_classes_to_dex(ss.str(),
                           &store.get_dexen()[i],
                           nullptr /* locator_index */,
                           i,
                           cfg,
                           pos_mapper.get(),
                           nullptr,
                           nullptr);
    }
  }
}
