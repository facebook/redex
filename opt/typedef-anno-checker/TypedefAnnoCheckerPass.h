/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "ConstantPropagationAnalysis.h"
#include "ControlFlow.h"
#include "Pass.h"
#include "TypeInference.h"

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
      ConcurrentMap<const DexClass*, std::unordered_set<std::string>>&
          strdef_constants,
      ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>>&
          intdef_constants);

  Config m_config;
  friend struct TypedefAnnoCheckerTest;
};

struct Stats {
  std::string m_error;
  size_t m_count{0};

  explicit Stats(std::string error) : m_error(std::move(error)), m_count(1) {}
  Stats() = default;

  Stats& operator+=(const Stats& other) {
    m_count += other.m_count;
    if (m_error.empty()) {
      m_error = other.m_error;
    }
    return *this;
  }
};

class TypedefAnnoChecker {
 public:
  explicit TypedefAnnoChecker(
      ConcurrentMap<const DexClass*, std::unordered_set<std::string>>&
          strdef_constants,
      ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>>&
          intdef_constants,
      const TypedefAnnoCheckerPass::Config& config)
      : m_config(config),
        m_strdef_constants(strdef_constants),
        m_intdef_constants(intdef_constants) {}

  void run(DexMethod* m);

  void check_instruction(
      DexMethod* m,
      type_inference::TypeInference* inference,
      type_inference::TypeEnvironment& env,
      IRInstruction* insn,
      IROpcode* opcode,
      const boost::optional<const DexType*>& return_annotation,
      const ConstantEnvironment& const_env);

  void check_typedef_value(DexMethod* m,
                           const type_inference::TypeEnvironment* env,
                           const boost::optional<const DexType*>& annotation,
                           reg_t reg,
                           const ConstantEnvironment& const_env);

  bool complete() { return m_good; }

  std::string error() { return m_error; }

 private:
  bool m_good{true};
  std::string m_error;
  TypedefAnnoCheckerPass::Config m_config;

  const ConcurrentMap<const DexClass*, std::unordered_set<std::string>>&
      m_strdef_constants;
  const ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>>&
      m_intdef_constants;
};
