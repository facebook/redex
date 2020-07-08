/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Configurable.h"
#include "InlinerConfig.h"

struct InlinerConfig : public inliner::InlinerConfig, Configurable {
 public:
  void bind_config() override;
  std::string get_config_name() override { return "InlinerConfig"; }
  std::string get_config_doc() override {
    return "This configuration is used to configure the inlinining which "
           "occurs in several Redex passes.";
  }
};

struct IRTypeCheckerConfig : public Configurable {
 public:
  void bind_config() override;
  std::string get_config_name() override { return "IRTypeCheckerConfig"; }
  std::string get_config_doc() override {
    return "This configuration is used to direct Redex to typecheck the IR "
           "after various stages of optimization.";
  }

  bool run_after_each_pass;
  bool verify_moves;
  std::unordered_set<std::string> run_after_passes;
  bool check_no_overwrite_this;
};

struct HasherConfig : public Configurable {
 public:
  void bind_config() override;
  std::string get_config_name() override { return "HasherConfig"; }
  std::string get_config_doc() override {
    return "This configuration is used to direct Redex to hash the contents of "
           "the dex"
           "after various stages of optimization to find non-determinism.";
  }

  bool run_after_each_pass;
};

struct CheckUniqueDeobfuscatedNamesConfig : public Configurable {
 public:
  void bind_config() override;
  std::string get_config_name() override {
    return "CheckUniqueDeobfuscatedNamesConfig";
  }
  std::string get_config_doc() override {
    return "This configuration is used to direct Redex to perform internal "
           "integrity checks.";
  }

  bool run_after_each_pass{false};
  bool run_initially{false};
  bool run_finally{false};
};

struct OptDecisionsConfig : public Configurable {
 public:
  void bind_config() override;
  std::string get_config_name() override { return "OptDecisionsConfig"; }
  std::string get_config_doc() override {
    return "This configuration is used to direct Redex if it should leave a "
           "log that explains the optimizations it has performed.";
  }

  bool enable_logs;
};

class GlobalConfig : public Configurable {

 public:
  void bind_config() override;
  std::string get_config_name() override { return "GlobalConfig"; }
  std::string get_config_doc() override {
    return "All the Redex configuration that isn't pass-specific lives here.";
  }

 private:
};
