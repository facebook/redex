/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexAnnotation.h"
#include "RedexContext.h"

#include <string>
#include <vector>

class DexIdx;
class DexMethod;
class DexMethodHandle;
class DexString;

class DexCallSite {
  friend struct RedexContext;

 public:
  DexCallSite(DexMethodHandle* linker_method_handle,
              DexString* linker_method_name,
              DexProto* linker_method_type,
              const std::vector<DexEncodedValue*>& linker_args) {
    m_linker_method_handle = linker_method_handle;
    m_linker_method_name = linker_method_name;
    m_linker_method_type = linker_method_type;
    m_linker_method_args = linker_args;
  }

 public:
  DexMethodHandle* method_handle() const { return m_linker_method_handle; }
  DexString* method_name() const { return m_linker_method_name; }
  DexProto* method_type() const { return m_linker_method_type; }
  const std::vector<DexEncodedValue*>& args() const {
    return m_linker_method_args;
  }

  void gather_strings(std::vector<DexString*>& lstring) const;
  void gather_methodhandles(std::vector<DexMethodHandle*>& lmethodhandle) const;
  void gather_methods(std::vector<DexMethodRef*>& lmethod) const;
  void gather_fields(std::vector<DexFieldRef*>& lfield) const;

  DexEncodedValueArray as_encoded_value_array() const;

 private:
  DexMethodHandle* m_linker_method_handle;
  DexString* m_linker_method_name;
  DexProto* m_linker_method_type;
  std::vector<DexEncodedValue*> m_linker_method_args;
};
