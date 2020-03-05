/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <iostream>
#include <string>

inline void open_or_die(const std::string& filename, std::ofstream* os) {
  os->open(filename);
  if (!os->is_open()) {
    std::cerr << "Unable to open: " << filename << std::endl;
    exit(EXIT_FAILURE);
  }
}
