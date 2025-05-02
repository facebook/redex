/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "ControlFlow.h"
#include "LiveRange.h"
#include "MethodOverrideGraph.h"
#include "Pass.h"
#include "TypeInference.h"

using TypeEnvironments =
    std::unordered_map<const IRInstruction*, type_inference::TypeEnvironment>;

namespace mog = method_override_graph;

/*
 * This pass checks that typedef annotations usages are value safe
 * https://developer.android.com/studio/write/annotations#enum-annotations
 */
class TypedefAnnoCheckerPass : public Pass {
 public:
  TypedefAnnoCheckerPass() : Pass("TypedefAnnoCheckerPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    return redex_properties::simple::preserves_all();
  }

  struct Config {
    DexType* int_typedef{nullptr};
    DexType* str_typedef{nullptr};
  };

  void bind_config() override {
    bind("int_typedef", {}, m_config.int_typedef);
    bind("str_typedef", {}, m_config.str_typedef);
  }

  explicit TypedefAnnoCheckerPass(Config config)
      : Pass("TypedefAnnoCheckerPass"), m_config(config) {}

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;

 private:
  void gather_typedef_values(
      const DexClass* cls,
      InsertOnlyConcurrentMap<const DexClass*,
                              std::unordered_set<const DexString*>>&
          strdef_constants,
      InsertOnlyConcurrentMap<const DexClass*, std::unordered_set<uint64_t>>&
          intdef_constants);

  Config m_config;
  friend struct TypedefAnnoCheckerTest;
};

struct CheckerStats {
  std::string m_errors;
  size_t m_count{0};

  explicit CheckerStats(std::string error)
      : m_errors(std::move(error)), m_count(1) {}
  CheckerStats() = default;

  CheckerStats& operator+=(const CheckerStats& other) {
    m_count += other.m_count;
    if (m_errors.empty()) {
      m_errors = other.m_errors;
    } else {
      m_errors = m_errors + other.m_errors;
    }
    return *this;
  }
};

using StrDefConstants =
    InsertOnlyConcurrentMap<const DexClass*,
                            std::unordered_set<const DexString*>>;

using IntDefConstants =
    InsertOnlyConcurrentMap<const DexClass*, std::unordered_set<uint64_t>>;

class TypedefAnnoChecker {
 public:
  explicit TypedefAnnoChecker(
      const StrDefConstants& strdef_constants,
      const IntDefConstants& intdef_constants,
      const TypedefAnnoCheckerPass::Config& config,
      const method_override_graph::Graph& method_override_graph)
      : m_config(config),
        m_strdef_constants(strdef_constants),
        m_intdef_constants(intdef_constants),
        m_method_override_graph(method_override_graph) {}

  bool is_value_of_opt(const DexMethod* m);
  bool is_delegate(const DexMethod* m);

  void run(DexMethod* m);

  void check_instruction(
      DexMethod* m,
      const type_inference::TypeInference* inference,
      IRInstruction* insn,
      const boost::optional<const DexType*>& return_annotation,
      live_range::UseDefChains* ud_chains,
      TypeEnvironments& envs);

  bool check_typedef_value(DexMethod* m,
                           const boost::optional<const DexType*>& annotation,
                           live_range::UseDefChains* ud_chains,
                           IRInstruction* insn,
                           const src_index_t src,
                           const type_inference::TypeInference* inference,
                           TypeEnvironments& envs);

  bool complete() { return m_good; }

  std::string error() { return m_error; }

 private:
  void add_error(const std::string& error, bool double_newline = true);

  bool m_good{true};
  std::string m_error;
  TypedefAnnoCheckerPass::Config m_config;

  const StrDefConstants& m_strdef_constants;
  const IntDefConstants& m_intdef_constants;
  const method_override_graph::Graph& m_method_override_graph;
};
