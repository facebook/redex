/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "util.h"

#include <memory>

class OatFile {
 public:
  enum class Status {
    PARSE_SUCCESS,
    PARSE_UNKNOWN_VERSION,
    PARSE_BAD_MAGIC_NUMBER,
    PARSE_FAILURE
  };

  OatFile() = default;

  UNCOPYABLE(OatFile);
  virtual ~OatFile();

  // reads magic number, returns correct oat file implementation.
  static std::unique_ptr<OatFile> parse(ConstBuffer buf);

  virtual void print(bool dump_classes, bool dump_tables) = 0;

  virtual Status status() = 0;
};
