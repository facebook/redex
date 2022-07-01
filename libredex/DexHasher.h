/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This hashing functionality captures all details of a scope. By running this
 * after each pass, it makes it easy to find non-determinism build-over-build.
 * Look for the ~result~hash~ info that's added to each pass metrics.
 * We use boost hashes for performance.
 *
 */

#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

class DexClass;

using Scope = std::vector<DexClass*>;

namespace hashing {

std::string hash_to_string(size_t hash);

struct DexHash {
  size_t positions_hash;
  size_t registers_hash;
  size_t code_hash;
  size_t signature_hash;
};

class DexScopeHasher final {
 public:
  explicit DexScopeHasher(const Scope& scope) : m_scope(scope) {}
  DexHash run();

 private:
  const Scope& m_scope;
};

class DexClassHasher final {
 public:
  explicit DexClassHasher(DexClass* cls);
  DexHash run();
  void print(std::ostream&);

  ~DexClassHasher();

 private:
  struct Fwd;
  std::unique_ptr<Fwd> m_fwd;
};

void print_classes(std::ostream& output, const Scope& classes);

} // namespace hashing

std::ostream& operator<<(std::ostream&, const hashing::DexHash&);
