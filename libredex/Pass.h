/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "Configurable.h"
#include "DexStore.h"
#include "Traits.h"

class AnalysisUsage;
struct ConfigFiles;
class PassManager;

class Pass : public Configurable {
 public:
  enum Kind {
    TRANSFORMATION,
    ANALYSIS,
  };

  explicit Pass(const std::string& name, Kind kind = TRANSFORMATION);

  std::string name() const { return m_name; }

  std::string get_config_name() override { return name(); };

  bool is_analysis_pass() const { return m_kind == ANALYSIS; }

  virtual void destroy_analysis_result() {
    always_assert_log(m_kind != ANALYSIS,
                      "destroy_analysis_result not implemented for %s",
                      m_name.c_str());
  }

  /**
   * All passes' eval_pass are run, and then all passes' run_pass are run. This
   * allows each pass to evaluate its rules in terms of the original input,
   * without other passes changing the identity of classes. You should NOT
   * change anything in the dex stores in eval_pass. There is no protection
   * against doing so, this is merely a convention.
   */

  virtual void eval_pass(DexStoresVector& stores,
                         ConfigFiles& conf,
                         PassManager& /* mgr */) {}
  virtual void run_pass(DexStoresVector& stores,
                        ConfigFiles& conf,
                        PassManager& mgr) = 0;

  virtual void set_analysis_usage(AnalysisUsage& analysis_usage) const;

  Configurable::Reflection reflect() override;

 private:
  std::string m_name;
  Kind m_kind;
};

/**
 * In certain cases, a pass will need to operate on a fragment of code (e.g. a
 * package or a class prefix), either without requiring knowledge from the other
 * packages or not committing any changes. PartialPasses will be added a
 * `run_on_packages` config option automatically and this base class takes care
 * of building class scopes based on known DexStore names. If the
 * `run_on_packages` config is an empty set of class prefixes, the pass will
 * operate on the entire program.
 */

class PartialPass : public Pass {
 public:
  explicit PartialPass(const ::std::string& name) : Pass(name) {}
  void bind_config() final {
    bind("run_on_packages", {}, m_select_packages);
    bind_partial_pass_config();
  }
  virtual void bind_partial_pass_config() {}

  void run_pass(DexStoresVector& whole_program_stores,
                ConfigFiles& conf,
                PassManager& mgr) final;

  virtual void run_partial_pass(DexStoresVector& whole_program_stores,
                                Scope current_scope,
                                ConfigFiles& conf,
                                PassManager& mgr) = 0;

 protected:
  Scope build_class_scope_with_packages_config(const DexStoresVector& stores);

 private:
  std::unordered_set<std::string> m_select_packages;
};
