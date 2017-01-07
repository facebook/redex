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

struct DexMethodRef {
  /* Method Ref related members */
  DexType* cls = nullptr;
  DexString* name = nullptr;
  DexProto* proto = nullptr;
  DexMethodRef() = default;
  DexMethodRef(DexType* cls, DexString* name, DexProto* proto)
      : cls(cls), name(name), proto(proto) {}

  bool operator==(const DexMethodRef& r) const {
    return cls == r.cls && name == r.name && proto == r.proto;
  }
};

namespace std {

template <>
struct hash<DexMethodRef> {
  size_t operator()(const DexMethodRef& r) const {
    size_t seed = boost::hash<DexType*>()(r.cls);
    boost::hash_combine(seed, r.name);
    boost::hash_combine(seed, r.proto);
    return seed;
  }
};

}
