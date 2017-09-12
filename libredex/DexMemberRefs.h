/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <boost/functional/hash.hpp>

class DexType;
class DexString;
class DexProto;

/**
 * A specification for a method in order to look it up in the global cache
 * or to create it. Also used to modify an existing method reference.
 */
struct DexMethodSpec {
  /* Method related members */
  DexType* cls = nullptr;
  DexString* name = nullptr;
  DexProto* proto = nullptr;
  DexMethodSpec() = default;
  DexMethodSpec(DexType* c, DexString* n, DexProto* p)
      : cls(c), name(n), proto(p) {}

  bool operator==(const DexMethodSpec& r) const {
    return cls == r.cls && name == r.name && proto == r.proto;
  }
};

/**
 * A specification for a field in order to look it up in the global cache
 * or to create it. Also used to modify an existing field reference.
 */
struct DexFieldSpec {
  /* Field related members */
  DexType* cls = nullptr;
  DexString* name = nullptr;
  DexType* type = nullptr;

  DexFieldSpec() = default;
  DexFieldSpec(DexType* c, DexString* n, DexType* t)
      : cls(c), name(n), type(t) {}

  bool operator==(const DexFieldSpec& r) const {
    return cls == r.cls && name == r.name && type  == r.type;
  };
};

namespace std {

template <>
struct hash<DexMethodSpec> {
  size_t operator()(const DexMethodSpec& r) const {
    size_t seed = boost::hash<DexType*>()(r.cls);
    boost::hash_combine(seed, r.name);
    boost::hash_combine(seed, r.proto);
    return seed;
  }
};

template <>
struct hash<DexFieldSpec> {
  size_t operator()(const DexFieldSpec& r) const {
    size_t seed = boost::hash<DexType*>()(r.cls);
    boost::hash_combine(seed, r.name);
    boost::hash_combine(seed, r.type);
    return seed;
  }
};

}
