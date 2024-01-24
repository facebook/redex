/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <iosfwd>

#include "DexClass.h"
#include "ReferencedState.h"

namespace ir_meta_io {

void dump(const Scope& classes, const std::string& output_dir);

bool load(const std::string& input_dir);

class IRMetaIO {
 public:
  struct bit_rstate_t {
    ReferencedState::InnerStruct inner_struct;
  };
  static void serialize_rstate(const ReferencedState& rstate,
                               std::ofstream& ostrm);
  static void deserialize_rstate(const char** _ptr, ReferencedState& rstate);

  /**
   * Only serialize meta data of class/method/field if they are not default
   */
  template <typename T>
  static bool is_default_meta(const T* obj) {
    return obj->get_deobfuscated_name_or_empty() == show(obj) &&
           (!obj->rstate.inner_struct.is_class() ||
            (!obj->rstate.inner_struct.m_by_string &&
             !obj->rstate.inner_struct.m_is_serde)) &&
           !obj->rstate.inner_struct.m_by_resources &&
           !obj->rstate.inner_struct.m_keep &&
           !obj->rstate.inner_struct.m_assumenosideeffects &&
           !obj->rstate.inner_struct.m_whyareyoukeeping &&
           !obj->rstate.inner_struct.m_set_allowshrinking &&
           !obj->rstate.inner_struct.m_unset_allowshrinking &&
           !obj->rstate.inner_struct.m_set_allowobfuscation &&
           !obj->rstate.inner_struct.m_unset_allowobfuscation;
  }
};
} // namespace ir_meta_io
