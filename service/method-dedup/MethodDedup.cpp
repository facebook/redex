/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodDedup.h"

#include "IRCode.h"
#include "MethodReference.h"

namespace {

struct CodeAsKey {
  IRCode* code;

  explicit CodeAsKey(IRCode* c) : code(c) {}

  bool operator==(const CodeAsKey& other) const {
    return code->structural_equals(*other.code);
  }
};

struct CodeHasher {
  size_t operator()(const CodeAsKey& key) const {
    size_t result = 0;
    for (auto& mie : InstructionIterable(key.code)) {
      result ^= mie.insn->hash();
    }
    return result;
  }
};

using DuplicateMethods =
    std::unordered_map<CodeAsKey, MethodOrderedSet, CodeHasher>;

std::vector<MethodOrderedSet> get_duplicate_methods_simple(
    const MethodOrderedSet& methods) {
  DuplicateMethods duplicates;
  for (DexMethod* method : methods) {
    always_assert(method->get_code());
    duplicates[CodeAsKey(method->get_code())].emplace(method);
  }

  std::vector<MethodOrderedSet> result;
  for (auto& duplicate : duplicates) {
    result.push_back(std::move(duplicate.second));
  }

  return result;
}

} // namespace

namespace method_dedup {

std::vector<MethodOrderedSet> group_similar_methods(
    const std::vector<DexMethod*>& methods) {

  // Split based on size.
  std::unordered_map<size_t, std::unordered_set<DexMethod*>> size_to_methods;
  for (const auto& method : methods) {
    size_to_methods[method->get_code()->sum_opcode_sizes()].emplace(method);
  }

  std::vector<MethodOrderedSet> result;

  // Split based on signature (regardless of the method name).
  std::unordered_map<DexProto*, MethodOrderedSet> proto_to_methods;
  for (const auto& pair : size_to_methods) {
    proto_to_methods.clear();
    const auto& meths = pair.second;

    for (const auto& meth : meths) {
      proto_to_methods[meth->get_proto()].emplace(meth);
    }

    for (auto& same_proto : proto_to_methods) {
      result.emplace_back(std::move(same_proto.second));
    }
  }

  return result;
}

std::vector<MethodOrderedSet> group_identical_methods(
    const std::vector<DexMethod*>& methods) {
  std::vector<MethodOrderedSet> result;
  std::vector<MethodOrderedSet> same_protos = group_similar_methods(methods);

  // Find actual duplicates.
  for (const auto& same_proto : same_protos) {
    std::vector<MethodOrderedSet> duplicates =
        get_duplicate_methods_simple(same_proto);

    result.insert(result.end(), duplicates.begin(), duplicates.end());
  }

  return result;
}

bool are_methods_identical(const std::vector<DexMethod*>& methods) {
  return group_identical_methods(methods).size() == 1;
}

size_t dedup_methods_helper(
    const Scope& scope,
    const std::vector<DexMethod*>& to_dedup,
    std::vector<DexMethod*>& replacements,
    boost::optional<std::unordered_map<DexMethod*, MethodOrderedSet>>&
        new_to_old) {
  if (to_dedup.size() <= 1) {
    replacements = to_dedup;
    return 0;
  }
  size_t dedup_count = 0;
  auto grouped_methods = group_identical_methods(to_dedup);
  std::unordered_map<DexMethod*, DexMethod*> duplicates_to_replacement;
  for (auto& group : grouped_methods) {
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
            "dedup: group %d replacement %s",
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
    std::vector<DexMethod*>& replacements,
    boost::optional<std::unordered_map<DexMethod*, MethodOrderedSet>>&
        new_to_old) {
  size_t total_dedup_count = 0;
  auto to_dedup_temp = to_dedup;
  while (true) {
    TRACE(
        METH_DEDUP, 8, "dedup: static|non_virt input %d", to_dedup_temp.size());
    size_t dedup_count =
        dedup_methods_helper(scope, to_dedup_temp, replacements, new_to_old);
    total_dedup_count += dedup_count;
    TRACE(METH_DEDUP, 8, "dedup: static|non_virt dedupped %d", dedup_count);
    if (dedup_count == 0) {
      break;
    }
    to_dedup_temp = replacements;
    replacements = {};
  }
  return total_dedup_count;
}

} // namespace method_dedup
