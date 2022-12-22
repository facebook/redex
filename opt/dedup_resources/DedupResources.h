/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"
#include "androidfw/ResourceTypes.h"

/**
 * Finds resource identifiers whose metadata values are identical in all
 * configurations. Of a set of duplicates, the smallest resource identifier will
 * be considered the "canonical" version and all others will have their data
 * removed. Resource IDs throughout the application (whether it be dex code,
 * binary XML files, and resource table references) are then rewritten to refer
 * to the canonical and resource identifiers are compacted to save more space in
 * resources.arsc file.
 *
 * In order to increase the amount of duplicate rows in the resource table, file
 * paths are checked for equality too.
 *
 * NOTE:
 * As with other Android Resource optimizations, rewriting dex code to reflect
 * modified resource identifiers relies on changing static field values of the
 * associated R classes, so these values must NOT be inlined throughout the
 * program (either by Redex passes or by compiler options before Redex)!
 *
 * Config options:
 * If a resource cannot be removed, either individual names (or entire types)
 * can be disabled. Do this if there is any requirement on looking up resources
 * by their string name, i.e. android.content.res.Resources.getIdentifier() on a
 * non-canonical duplicate.
 *
 * Possible further improvements:
 * This does not do a particularly good job of finding duplicate files when run
 * against .aab inputs. Reason for this is that .pb schema for .xml files will
 * encode some trivial details, ostensibly for human readability that will not
 * matter for emitting the final .apk format of xml files. This makes our naive
 * hashing/file equivalence is not good enough to recognize true duplicates.
 * This unfortunately means that the output of this pass can be different when
 * run against equivalent .apk / .aab files. Ideally the pass would do the same
 * thing, but oh well not yet.
 */
class DedupResourcesPass : public Pass {
 public:
  DedupResourcesPass() : Pass("DedupResourcesPass") {}

  void bind_config() override {
    bind("disallowed_types", {}, m_disallowed_types);
    bind("disallowed_resources", {}, m_disallowed_resources);
  }

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  void prepare_disallowed_ids(const std::string&,
                              std::unordered_set<uint32_t>* disallowed_types,
                              std::unordered_set<uint32_t>* disallowed_ids);

  std::unordered_set<std::string> m_disallowed_types;
  std::unordered_set<std::string> m_disallowed_resources;
};
