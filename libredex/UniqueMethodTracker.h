/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <utility>

#include "ConcurrentContainers.h"
#include "ControlFlow.h"
#include "DexClass.h"

// Tracks unique method code patterns using hash + string equality.
// Thread-safe for concurrent inserts.
class UniqueMethodTracker {
 public:
  // Inserts a method and returns (representative, was_inserted).
  // - If the code is new, returns (method, true).
  // - If the code was seen before, returns (first_method_with_same_code,
  //   false).
  // - If the method has no code or CFG, returns (nullptr, false).
  std::pair<const DexMethod*, bool> insert(const DexMethod* method);

  size_t size() const { return m_unique_methods.size(); }

 protected:
  // Insert with a specific hash value. Exposed for testing collision handling.
  std::pair<const DexMethod*, bool> insert(const DexMethod* method,
                                           size_t code_hash);

 private:
  // Compare two CFGs instruction by instruction.
  static bool cfg_code_equals(const cfg::ControlFlowGraph& a,
                              const cfg::ControlFlowGraph& b) {
    auto iter_a = cfg::ConstInstructionIterable(a);
    auto iter_b = cfg::ConstInstructionIterable(b);
    return std::equal(iter_a.begin(), iter_a.end(), iter_b.begin(),
                      iter_b.end(), [](const auto& mie_a, const auto& mie_b) {
                        return *mie_a.insn == *mie_b.insn;
                      });
  }

  struct Key {
    size_t code_hash;
    const DexMethod* method;

    bool operator==(const Key& other) const {
      if (code_hash != other.code_hash) {
        return false;
      }
      // Defensive: verify both methods still have valid code/CFG.
      const auto* const code = method->get_code();
      const auto* const other_code = other.method->get_code();
      if (code == nullptr || other_code == nullptr || !code->cfg_built() ||
          !other_code->cfg_built()) {
        return false;
      }
      // Compare instruction by instruction on collision; storing serialized
      // method bodies may explode memory usage.
      return cfg_code_equals(code->cfg(), other_code->cfg());
    }
  };

  struct KeyHash {
    size_t operator()(const Key& key) const { return key.code_hash; }
  };

  InsertOnlyConcurrentSet<Key, KeyHash> m_unique_methods;
};
