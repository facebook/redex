/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <iostream>

#include "MethodProfiles.h"
#include "RedexContext.h"

int main(int argc, char* argv[]) {
  if (argc == 1 || std::string("--help") == argv[1] ||
      std::string("-h") == argv[1]) {
    // No args (or help), print usage.
    std::cerr << "Usage: check-method-profiles PROF-FILE [PROF-FILE...]"
              << std::endl;
    return argc == 1 ? 1 : 0;
  }

  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    files.push_back(argv[i]);
  }

  RedexContext rc;
  g_redex = &rc;
  method_profiles::MethodProfiles m;
  m.initialize(files);
  g_redex = nullptr;
}
