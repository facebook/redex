/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
#include <memory>
#include <ostream>
#include <string>

#include "Debug.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "Sha1.h"

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
  explicit DexClassHasher(DexClass* cls) : m_cls(cls) {}
  DexHash run();

 private:
  void hash(const std::string& str);
  void hash(int value);
  void hash(uint64_t value);
  void hash(uint32_t value);
  void hash(uint16_t value);
  void hash(uint8_t value);
  void hash(bool value);
  void hash(const IRCode* c);
  void hash(const IRInstruction* insn);
  void hash(const EncodedAnnotations* a);
  void hash(const ParamAnnotations* m);
  void hash(const DexAnnotation* a);
  void hash(const DexAnnotationSet* s);
  void hash(const DexAnnotationElement& elem);
  void hash(const DexEncodedValue* v);
  void hash(const DexProto* p);
  void hash(const DexMethodRef* m);
  void hash(const DexMethod* m);
  void hash(const DexFieldRef* f);
  void hash(const DexField* f);
  void hash(const DexType* t);
  void hash(const DexTypeList* l);
  void hash(const DexString* s);
  template <class T>
  void hash(const std::vector<T>& l) {
    hash((uint64_t)l.size());
    for (const auto& elem : l) {
      hash(elem);
    }
  }
  template <class T>
  void hash(const std::deque<T>& l) {
    hash((uint64_t)l.size());
    for (const auto& elem : l) {
      hash(elem);
    }
  }
  template <typename T>
  void hash(const std::unique_ptr<T>& uptr) {
    hash(uptr.get());
  }
  template <class K, class V>
  void hash(const std::map<K, V>& l) {
    hash((uint64_t)l.size());
    for (const auto& p : l) {
      hash(p.first);
      hash(p.second);
    }
  }
  DexClass* m_cls;
  size_t m_hash{0};
  size_t m_code_hash{0};
  size_t m_registers_hash{0};
  size_t m_positions_hash{0};
};

} // namespace hashing
