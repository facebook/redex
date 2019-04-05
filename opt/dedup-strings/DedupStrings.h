/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "InterDexPass.h"
#include "Pass.h"
#include "PluginRegistry.h"

class DedupStrings {
 public:
  struct Stats {
    size_t load_strings_within_try{0};
    size_t perf_sensitive_strings{0};
    size_t non_perf_sensitive_strings{0};
    size_t perf_sensitive_methods{0};
    size_t non_perf_sensitive_methods{0};
    size_t excluded_duplicate_non_load_strings{0};
    size_t duplicate_strings{0};
    size_t duplicate_strings_size{0};
    size_t duplicate_string_loads{0};
    size_t expected_size_reduction{0};
    size_t dexes_without_host_cls{0};
    size_t factory_methods{0};
  };

  DedupStrings(
      size_t max_factory_methods,
      bool use_method_to_weight,
      const std::unordered_map<std::string, unsigned int>& method_to_weight)
      : m_max_factory_methods(max_factory_methods),
        m_use_method_to_weight(use_method_to_weight),
        m_method_to_weight(method_to_weight) {}

  const Stats& get_stats() const { return m_stats; }

  void run(DexStoresVector& stores);

 private:
  struct DedupStringInfo {
    size_t duplicate_string_loads;
    std::unordered_set<size_t> dexes_to_dedup;

    uint32_t index{0xFFFFFFFF};
    DexMethod* const_string_method{nullptr};
  };

  std::vector<DexClass*> get_host_classes(DexClassesVector& dexen);
  std::unordered_map<const DexMethod*, size_t> get_methods_to_dex(
      const DexClassesVector& dexen);
  std::unordered_set<const DexMethod*> get_perf_sensitive_methods(
      const DexClassesVector& dexen);
  DexMethod* make_const_string_loader_method(
      DexClass* host_cls, const std::vector<DexString*>& strings);
  void gather_non_load_strings(DexClasses& classes,
                               std::unordered_set<const DexString*>* strings);
  void gather_load_strings_within_try(
      DexClasses& classes, std::unordered_set<const DexString*>* strings);
  ConcurrentMap<DexString*, std::unordered_map<size_t, size_t>> get_occurrences(
      const Scope& scope,
      const std::unordered_map<const DexMethod*, size_t>& methods_to_dex,
      const std::unordered_set<const DexMethod*>& perf_sensitive_methods,
      std::unordered_set<const DexString*> non_load_strings[]);
  std::unordered_map<DexString*, DedupStringInfo> get_strings_to_dedup(
      DexClassesVector& dexen,
      const ConcurrentMap<DexString*, std::unordered_map<size_t, size_t>>&
          occurrences,
      std::unordered_map<const DexMethod*, size_t>& methods_to_dex,
      std::unordered_set<const DexMethod*>& perf_sensitive_methods,
      const std::unordered_set<const DexString*> non_load_strings[]);
  void rewrite_const_string_instructions(
      const Scope& scope,
      const std::unordered_map<const DexMethod*, size_t>& methods_to_dex,
      const std::unordered_set<const DexMethod*>& perf_sensitive_methods,
      const std::unordered_map<DexString*, DedupStringInfo>& strings_to_dedup);

  mutable Stats m_stats;
  size_t m_max_factory_methods;
  bool m_use_method_to_weight;
  std::unordered_map<std::string, unsigned int> m_method_to_weight;
};

/**
 * This pass de-duplicates strings across dexes when this would decrease overall
 * size.
 *
 * Without this pass, if a string is used in multiple dexes, it would be
 * separately embedded in all those different dexes. This results in wasted
 * space on disk, even after compression.
 *
 * This pass de-duplicates those string across dexes for which this would result
 * in a decrease in code size:
 * - A particular dex is chosen to host a string --- the dex which references
 *   the string most often in const-string instructions
 * - A dispatcher function is introduced in that dex. It roughly has the
 *   following form:
 *
 *     static String $const$string(int id) {
 *       switch (id) {
 *         case 0: return "string_0";
 *         ...
 *         default: // case n-1
 *           return "string_n_minus_1";
 *       }
 *     }
 *
 * - References to the string from other dexes (except the primary dex and other
 *   perf sensitive classes) are rewritten to invoke the hosting function.
 *   An instruction like
 *
 *     const-string v0, "foo"
 *
 *   turns into
 *
 *     const v0, 123 // index of "foo" in some hosting dex
 *     invoke-static {v0}, $const-string // of hosting dex
 *     move-result-object v0
 *
 * - If a dex also refers to the string separately from const-string
 *   instructions, then the string does not participate in the de-duplication
 *   logic, as it's not possible to de-dup it anyway.
 * - References from the primary dex are not rewritten, as the primary dex may
 *   not include forward references to other dexes. Also, perf sensitive
 *   classes, which are those used for cold start or mixed mode as determined
 *   by the InterDex pass, are not rewritten.
 * - We perform a benefits/costs analysis for each string:
 *   - Dropping a string from a dex will save a string table entry, which
 *     consists of an encoding of the length of the string, plus the MUTF8
 *     encoding of the string itself, plus a 4 byte index into the table.
 *   - The hosting function will need around 10 bytes for each switch case.
 *   - Rewriting a const-string reference into a hosting function invocation
 *     adds 8 bytes. (Sometimes less, if we can condense a const-string/jumbo,
 *     or if the new index fits into fewer bits.)
 *
 * Besides the space savings, there are other perf implications:
 * - The string tables shrink; this is probably good, as they likely tend to
 *   be kept in memory, e.g. due to type locator look-ups.
 * - De-duped strings need to get interned less often by the VM (they are
 *   interned on first access), and the VM will store less metadata. This should
 *   be good.
 * - De-duped string look-ups from other dexes become slightly more expensive,
 *   due to the dispatcher indirection.
 *
 * This pass should run at the very end of all passes, certainly after the
 * inter-dex pass, but before the replace-gotos-with-returns pass.
 */
class DedupStringsPass : public Pass {
 public:
  DedupStringsPass() : Pass("DedupStringsPass") {}

  virtual void configure_pass(const JsonWrapper& jw) override;
  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  size_t m_max_factory_methods;
  bool m_use_method_to_weight;
};
