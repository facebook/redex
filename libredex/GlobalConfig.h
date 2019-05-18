/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Configurable.h"

struct OptDecisionsConfig : public Configurable {
 public:
  void bind_config() override;
  std::string get_config_name() override { return "OptDecisionsConfig"; };
  std::string get_config_doc() override {
    return "This configuration is used to direct Redex if it should leave a "
           "log that explains the optimizations it has performed.";
  }

  bool enable_logs;
  std::string output_file_name;
};

class GlobalConfig : public Configurable {

 public:
  void bind_config() override;
  std::string get_config_name() override { return "GlobalConfig"; };
  std::string get_config_doc() override {
    return "All the Redex configuration that isn't pass-specific lives here.";
  }

 private:
  OptDecisionsConfig m_opt_decisions_config;
};
