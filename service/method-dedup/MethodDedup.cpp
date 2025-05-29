/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodDedup.h"

#include "DedupBlocks.h"
#include "DexOpcode.h"
#include "IRCode.h"
#include "MethodReference.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {

struct CodeAsKey {
  const cfg::ControlFlowGraph& cfg;
  const bool dedup_fill_in_stack_trace;

  CodeAsKey(cfg::ControlFlowGraph& c, bool dedup_fill_in_stack_trace)
      : cfg(c), dedup_fill_in_stack_trace(dedup_fill_in_stack_trace) {}

  static bool non_throw_instruction_equal(const IRInstruction& left,
                                          const IRInstruction& right) {
    return left == right &&
           !dedup_blocks_impl::is_ineligible_because_of_fill_in_stack_trace(
               &left);
  }

  bool operator==(const CodeAsKey& other) const {
    return dedup_fill_in_stack_trace
               ? cfg.structural_equals(other.cfg)
               : cfg.structural_equals(other.cfg, non_throw_instruction_equal);
  }
};

struct CodeHasher {
  size_t operator()(const CodeAsKey& key) const {
    size_t result = 0;
    for (auto& mie : cfg::InstructionIterable(
             const_cast<cfg::ControlFlowGraph&>(key.cfg))) {
      result ^= mie.insn->hash();
    }
    return result;
  }
};

using DuplicateMethods = UnorderedMap<CodeAsKey, MethodOrderedSet, CodeHasher>;

UnorderedBag<MethodOrderedSet> get_duplicate_methods_simple(
    const MethodOrderedSet& methods, bool dedup_fill_in_stack_trace) {
  DuplicateMethods duplicates;
  for (DexMethod* method : methods) {
    always_assert(method->get_code());
    always_assert(method->get_code()->editable_cfg_built());
    duplicates[CodeAsKey(method->get_code()->cfg(), dedup_fill_in_stack_trace)]
        .emplace(method);
  }

  UnorderedBag<MethodOrderedSet> result;
  for (auto& duplicate : UnorderedIterable(duplicates)) {
    result.insert(std::move(duplicate.second));
  }

  return result;
}

} // namespace

namespace method_dedup {

UnorderedBag<MethodOrderedSet> group_similar_methods(
    const std::vector<DexMethod*>& methods) {

  // Split based on size.
  UnorderedMap<size_t, UnorderedSet<DexMethod*>> size_to_methods;
  for (const auto& method : methods) {
    size_to_methods[method->get_code()->estimate_code_units()].emplace(method);
  }

  UnorderedBag<MethodOrderedSet> result;

  // Split based on signature (regardless of the method name).
  UnorderedMap<DexProto*, MethodOrderedSet> proto_to_methods;
  for (const auto& pair : UnorderedIterable(size_to_methods)) {
    proto_to_methods.clear();
    const auto& meths = pair.second;

    for (const auto& meth : UnorderedIterable(meths)) {
      proto_to_methods[meth->get_proto()].emplace(meth);
    }

    for (auto& same_proto : UnorderedIterable(proto_to_methods)) {
      result.insert(std::move(same_proto.second));
    }
  }

  return result;
}

UnorderedBag<MethodOrderedSet> group_identical_methods(
    const std::vector<DexMethod*>& methods, bool dedup_fill_in_stack_trace) {
  UnorderedBag<MethodOrderedSet> result;
  UnorderedBag<MethodOrderedSet> same_protos = group_similar_methods(methods);

  // Find actual duplicates.
  for (const auto& same_proto : UnorderedIterable(same_protos)) {
    UnorderedBag<MethodOrderedSet> duplicates =
        get_duplicate_methods_simple(same_proto, dedup_fill_in_stack_trace);
    insert_unordered_iterable(result, duplicates);
  }

  return result;
}

/**
 * This method is for testing the method deduplicating logic. The
 * dedup_fill_in_stack_trace flag is passed down to the method-deduping.
 */
bool are_deduplicatable(const std::vector<DexMethod*>& methods,
                        bool dedup_fill_in_stack_trace) {
  return group_identical_methods(methods, dedup_fill_in_stack_trace).size() ==
         1;
}

size_t dedup_methods_helper(
    const Scope& scope,
    const std::vector<DexMethod*>& to_dedup,
    bool dedup_fill_in_stack_trace,
    std::vector<DexMethod*>& replacements,
    boost::optional<UnorderedMap<DexMethod*, MethodOrderedSet>>& new_to_old) {
  if (to_dedup.size() <= 1) {
    replacements = to_dedup;
    return 0;
  }
  size_t dedup_count = 0;
  auto grouped_methods =
      group_identical_methods(to_dedup, dedup_fill_in_stack_trace);
  UnorderedMap<DexMethod*, DexMethod*> duplicates_to_replacement;
  for (auto& group : UnorderedIterable(grouped_methods)) {
    auto replacement = *group.begin();
    for (auto m : group) {
      if (m != replacement) {
        duplicates_to_replacement[m] = replacement;
      }
      // Update dedup map
      if (new_to_old == boost::none) {
        continue;
      }
      if (new_to_old->count(m) > 0) {
        auto orig_old_list = new_to_old->at(m);
        new_to_old->erase(m);
        for (auto orig_old : orig_old_list) {
          new_to_old.get()[replacement].insert(orig_old);
        }
      }
      new_to_old.get()[replacement].insert(m);
    }
    if (new_to_old != boost::none) {
      new_to_old.get()[replacement].insert(replacement);
    }

    replacements.push_back(replacement);
    if (group.size() > 1) {
      dedup_count += group.size() - 1;
      TRACE(METH_DEDUP,
            9,
            "dedup: group %zu replacement %s",
            group.size(),
            SHOW(replacement));
    }
  }
  method_reference::update_call_refs_simple(scope, duplicates_to_replacement);
  return dedup_count;
}

size_t dedup_methods(
    const Scope& scope,
    const std::vector<DexMethod*>& to_dedup,
    bool dedup_fill_in_stack_trace,
    std::vector<DexMethod*>& replacements,
    boost::optional<UnorderedMap<DexMethod*, MethodOrderedSet>>& new_to_old) {
  size_t total_dedup_count = 0;
  auto to_dedup_temp = to_dedup;
  while (true) {
    TRACE(METH_DEDUP,
          8,
          "dedup: static|non_virt input %zu",
          to_dedup_temp.size());
    size_t dedup_count = dedup_methods_helper(scope,
                                              to_dedup_temp,
                                              dedup_fill_in_stack_trace,
                                              replacements,
                                              new_to_old);
    total_dedup_count += dedup_count;
    TRACE(METH_DEDUP, 8, "dedup: static|non_virt dedupped %zu", dedup_count);
    if (dedup_count == 0) {
      break;
    }
    to_dedup_temp = replacements;
    replacements = {};
  }
  return total_dedup_count;
}

} // namespace method_dedup
