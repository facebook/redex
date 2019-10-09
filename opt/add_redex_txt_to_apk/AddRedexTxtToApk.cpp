/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AddRedexTxtToApk.h"

#include <string>

#include "Debug.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "Pass.h"
#include "Warning.h"

void AddRedexTxtToApkPass::run_pass(DexStoresVector& /* unused */,
                                    ConfigFiles& conf,
                                    PassManager& /* unused */) {
  std::string apk_dir;
  conf.get_json_config().get("apk_dir", "", apk_dir);

  if (!apk_dir.size()) {
    TRACE(ADD_REDEX_TXT, 2, "apk_dir not set, so not writing redex.txt\n");
    return;
  }
  std::string out_file_name = apk_dir + "/redex.txt";
  std::ofstream redex_txt(out_file_name);
  if (redex_txt.is_open()) {
    redex_txt << "Optimized by Redex" << std::endl
              << "http://fbredex.com/" << std::endl;
    redex_txt.close();
  } else {
    opt_warn(CANT_WRITE_FILE, "Unable to write file %s\n", out_file_name.c_str());
  }
}

static AddRedexTxtToApkPass s_pass;
