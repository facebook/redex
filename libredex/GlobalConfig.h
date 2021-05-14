/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>

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
  bool check_num_of_refs;
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

struct AssessorConfig : public Configurable {
 public:
  void bind_config() override;
  std::string get_config_name() override { return "AssessorConfig"; }
  std::string get_config_doc() override {
    return "This configuration is used to direct Redex to perform internal "
           "quality assessments.";
  }

  bool run_after_each_pass{false};
  bool run_initially{false};
  bool run_finally{false};
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

class GlobalConfig;

using BindOperationFn = std::function<std::unique_ptr<Configurable>(
    GlobalConfig*, const std::string&)>;

struct GlobalConfigRegistryEntry {
  GlobalConfigRegistryEntry(const std::string& name,
                            BindOperationFn bind_operation);
  std::string name;
  BindOperationFn bind_operation;
};

using GlobalConfigRegistry = std::vector<GlobalConfigRegistryEntry>;

class GlobalConfig : public Configurable {
 public:
  explicit GlobalConfig(GlobalConfigRegistry registry)
      : m_registry(std::move(registry)) {}

  void bind_config() override;
  std::string get_config_name() override { return "GlobalConfig"; }
  std::string get_config_doc() override {
    return "All the Redex configuration that isn't pass-specific lives here.";
  }

  template <typename ConfigType>
  ConfigType* get_config_by_name(const std::string& name) const {
    auto& type = m_global_configs.at(name);
    return static_cast<ConfigType*>(type.get());
  }

  template <typename ConfigType>
  static BindOperationFn get_bind_operation() {
    return [](GlobalConfig* global_config,
              const std::string& name) -> std::unique_ptr<Configurable> {
      std::unique_ptr<ConfigType> config_ptr = std::make_unique<ConfigType>();
      auto& config = *config_ptr;
      global_config->bind(
          name, ConfigType(), config, config_ptr->get_config_doc());
      return std::unique_ptr<Configurable>{config_ptr.release()};
    };
  }

  template <typename ConfigType>
  static GlobalConfigRegistryEntry register_as(const std::string& name) {
    return GlobalConfigRegistryEntry(name, get_bind_operation<ConfigType>());
  }

  static GlobalConfigRegistry& default_registry();

 private:
  std::unordered_map<std::string, std::unique_ptr<Configurable>>
      m_global_configs;
  GlobalConfigRegistry m_registry;
};
