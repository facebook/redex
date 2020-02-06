/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "Reachability.h"

class RemoveUnreachablePass : public Pass {
 public:
  RemoveUnreachablePass() : Pass("RemoveUnreachablePass") {}

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

  void write_out_removed_symbols(
      const std::string& filepath,
      const ConcurrentSet<std::string>& removed_symbols);

 private:
  reachability::IgnoreSets m_ignore_sets;
  boost::optional<uint32_t> m_emit_graph_on_run;
};
