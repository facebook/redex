/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "Reachability.h"

class RemoveUnreachablePassBase : public Pass {
 public:
  explicit RemoveUnreachablePassBase(const std::string& name) : Pass(name) {}

  void bind_config() override {
    bind("ignore_string_literals", {}, m_ignore_sets.string_literals);
    bind("ignore_string_literal_annos", {}, m_ignore_sets.string_literal_annos);
    bind("ignore_system_annos", {}, m_ignore_sets.system_annos);
    bind("keep_class_in_string", true, m_ignore_sets.keep_class_in_string);
    bind("emit_graph_on_run", boost::optional<uint32_t>{}, m_emit_graph_on_run);
    after_configuration([this] {
      // To keep the backward compatability of this code, ensure that the
      // "MemberClasses" annotation is always in system_annos.
      m_ignore_sets.system_annos.emplace(
          DexType::get_type("Ldalvik/annotation/MemberClasses;"));
    });
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  virtual std::unique_ptr<reachability::ReachableObjects>
  compute_reachable_objects(const DexStoresVector& stores,
                            PassManager& pm,
                            int* num_ignore_check_strings,
                            bool emit_graph_this_run) = 0;

  void write_out_removed_symbols(
      const std::string& filepath,
      const ConcurrentSet<std::string>& removed_symbols);

 protected:
  reachability::IgnoreSets m_ignore_sets;
  boost::optional<uint32_t> m_emit_graph_on_run;
};

class RemoveUnreachablePass : public RemoveUnreachablePassBase {
 public:
  RemoveUnreachablePass()
      : RemoveUnreachablePassBase("RemoveUnreachablePass") {}

  std::unique_ptr<reachability::ReachableObjects> compute_reachable_objects(
      const DexStoresVector& stores,
      PassManager& pm,
      int* num_ignore_check_strings,
      bool emit_graph_this_run) override;
};
