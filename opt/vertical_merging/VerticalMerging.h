/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DeterministicContainers.h"
#include "DexClass.h"
#include "Pass.h"

/**
 * Merge classes vertically. (see below situation)
 * Abstract class A          class C             class E
 *     |                       |                     |
 *  class B                  class D             class F
 * If A only has one child B, then B can be merged into A
 * If C only has one child D, and C is not referenced anywhere in code,
 * then C can be merged into D.
 * If class E only has one child F, and F is not referenced anywhere in code,
 * then F can be merged into E.
 */
class VerticalMergingPass : public Pass {
 public:
  VerticalMergingPass() : Pass("VerticalMergingPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {NoResolvablePureRefs, Preserves},
        {NeedsEverythingPublic, Establishes}, // TT150850158
    };
  }

  void bind_config() override { bind("blocklist", {}, m_blocklist); }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  void merge_classes(const Scope&, const UnorderedMap<DexClass*, DexClass*>&);

  void move_methods(DexClass*,
                    DexClass*,
                    bool,
                    UnorderedMap<DexMethodRef*, DexMethodRef*>*);
  void change_super_calls(const UnorderedMap<DexClass*, DexClass*>&);
  void change_init_calls(const Scope&,
                         const UnorderedMap<DexClass*, DexClass*>&);
  std::vector<std::string> m_blocklist;
};
