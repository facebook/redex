/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexCallSite.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "DexIdx.h"
#include "DexMethodHandle.h"

#include <sstream>

void DexCallSite::gather_strings(std::vector<DexString*>& lstring) const {
  lstring.emplace_back(m_linker_method_name);
  m_linker_method_type->gather_strings(lstring);
  for (auto ev : m_linker_method_args) {
    ev->gather_strings(lstring);
  }
}

void DexCallSite::gather_methodhandles(
    std::vector<DexMethodHandle*>& lmethodhandle) const {
  lmethodhandle.push_back(m_linker_method_handle);
  for (auto ev : m_linker_method_args) {
    ev->gather_methodhandles(lmethodhandle);
  }
}

void DexCallSite::gather_methods(std::vector<DexMethodRef*>& lmethod) const {
  m_linker_method_handle->gather_methods(lmethod);
  for (auto ev : m_linker_method_args) {
    ev->gather_methods(lmethod);
  }
}

void DexCallSite::gather_fields(std::vector<DexFieldRef*>& lfield) const {
  m_linker_method_handle->gather_fields(lfield);
  for (auto ev : m_linker_method_args) {
    ev->gather_fields(lfield);
  }
}

DexEncodedValueArray DexCallSite::as_encoded_value_array() const {
  auto aev = std::make_unique<std::deque<DexEncodedValue*>>();
  for (auto arg : m_linker_method_args) {
    aev->push_back(arg);
  }
  aev->push_front(new DexEncodedValueMethodType(m_linker_method_type));
  aev->push_front(new DexEncodedValueString(m_linker_method_name));
  aev->push_front(new DexEncodedValueMethodHandle(m_linker_method_handle));
  return DexEncodedValueArray(aev.release(), true);
}
