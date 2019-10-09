/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Vinfo.h"

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "Resolver.h"

namespace {

using methods_t = Vinfo::methods_t;
using vinfo_t = Vinfo::vinfo_t;
using vinfos_t = Vinfo::vinfos_t;

void build_vinfos_for_meth(vinfos_t& vinfos, const DexMethod* meth) {
  // Get super method
  auto cls = type_class(meth->get_class());
  const DexMethod* super_meth = cls == nullptr ? nullptr :
      resolve_virtual(type_class(cls->get_super_class()),
          meth->get_name(), meth->get_proto());
  // If we have a super method, we're an override, and it's overriden
  if (super_meth) {
    vinfos[meth].override_of = super_meth;
    vinfos[super_meth].overriden_by.insert(meth);
    vinfos[super_meth].is_overriden = true;
  }
  const DexMethod* decl = find_top_impl(
      cls, meth->get_name(), meth->get_proto());
  vinfos[meth].decl = decl;
}

vinfos_t build_vinfos(const std::vector<DexClass*>& scope) {
  vinfos_t vinfos;
  for (const DexClass* cls : scope) {
    if (is_interface(cls)) continue;
    for (const DexMethod* meth : cls->get_vmethods()) {
      build_vinfos_for_meth(vinfos, meth);
    }
  }
  return vinfos;
}

} // end namespace

Vinfo::Vinfo(const std::vector<DexClass*>& scope) {
  m_vinfos = build_vinfos(scope);
}

const DexMethod* Vinfo::get_decl(const DexMethod* meth) {
  redex_assert(m_vinfos.find(meth) != m_vinfos.end());
  return m_vinfos[meth].decl;
}

bool Vinfo::is_override(const DexMethod* meth) {
  redex_assert(m_vinfos.find(meth) != m_vinfos.end());
  return m_vinfos[meth].override_of;
}

const DexMethod* Vinfo::get_overriden_method(const DexMethod* meth) {
  redex_assert(m_vinfos.find(meth) != m_vinfos.end());
  return m_vinfos[meth].override_of;
}

bool Vinfo::is_overriden(const DexMethod* meth) {
  redex_assert(m_vinfos.find(meth) != m_vinfos.end());
  return m_vinfos[meth].is_overriden;
}

const methods_t& Vinfo::get_override_methods(const DexMethod* meth) {
  redex_assert(m_vinfos.find(meth) != m_vinfos.end());
  return m_vinfos[meth].overriden_by;
}
