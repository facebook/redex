/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexClass.h"
#include "ReferencedState.h"

namespace ir_meta_io {

void dump(const Scope& classes, const std::string& output_dir);

bool load(const std::string& output_dir, RedexContext* rdx_context);

class IRMetaIO {
 public:
  static void serialize_rstate(const ReferencedState& rstate,
                               std::ofstream& ostrm);

  /**
   * Only serialize meta data of class/method/field if they are not default
   */
  template <typename T>
  static bool is_default_meta(const T* obj) {
    return obj->get_deobfuscated_name() == show(obj) && !obj->rstate.m_bytype &&
           !obj->rstate.m_bystring && !obj->rstate.m_byresources &&
           !obj->rstate.m_mix_mode && !obj->rstate.m_keep &&
           !obj->rstate.m_assumenosideeffects &&
           !obj->rstate.m_blanket_keepnames &&
           !obj->rstate.m_whyareyoukeeping &&
           !obj->rstate.m_set_allowshrinking &&
           !obj->rstate.m_unset_allowshrinking &&
           !obj->rstate.m_set_allowobfuscation &&
           !obj->rstate.m_unset_allowobfuscation && !obj->rstate.m_keep_name &&
           !obj->rstate.m_keep_count.load();
  }
};
} // namespace ir_meta_io
