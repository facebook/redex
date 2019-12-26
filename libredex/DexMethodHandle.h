/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "RedexContext.h"

#include "DexDefs.h"
#include <string>
#include <vector>

class DexType;

class DexMethodHandle {
  friend struct RedexContext;

 public:
  DexMethodHandle(MethodHandleType type, DexMethodRef* methodref);
  DexMethodHandle(MethodHandleType type, DexFieldRef* fieldref);

  MethodHandleType type() const { return m_type; }
  DexMethodRef* methodref() const;
  DexFieldRef* fieldref() const;

  static bool isInvokeType(MethodHandleType type);

 private:
  MethodHandleType m_type;
  union {
    DexFieldRef* m_fieldref;
    DexMethodRef* m_methodref;
  };
};
