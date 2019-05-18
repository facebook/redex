/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Configurable.h"

class GlobalConfig : public Configurable {

 public:
  void bind_config() override;
  std::string get_config_name() override { return "GlobalConfig"; };
  std::string get_config_doc() override {
    return "All the Redex configuration that isn't pass-specific lives here.";
  }

 private:
};
