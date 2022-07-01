/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Resolver.h"
#include "DexUtil.h"

namespace {

inline bool match(const DexString* name,
                  const DexProto* proto,
                  const DexMethod* cls_meth) {
  return name == cls_meth->get_name() && proto == cls_meth->get_proto();
}

DexMethod* resolve_intf_method_ref(const DexClass* cls,
                                   const DexString* name,
                                   const DexProto* proto) {

  auto find_method = [&](const DexClass* cls) -> DexMethod* {
    const auto& vmethods = cls->get_vmethods();
    for (const auto vmethod : vmethods) {
      if (vmethod->get_name() == name && vmethod->get_proto() == proto) {
        return vmethod;
      }
    }
    return nullptr;
  };

  auto method = find_method(cls);
  if (method) return method;
  const auto& super_intfs = cls->get_interfaces()->get_type_list();
  for (const auto& super_intf : super_intfs) {
    const auto& super_intf_cls = type_class(super_intf);
    if (super_intf_cls == nullptr) continue;
    method = resolve_intf_method_ref(super_intf_cls, name, proto);
    if (method) return method;
  }
  return nullptr;
}

} // namespace

DexMethod* resolve_method(const DexClass* cls,
                          const DexString* name,
                          const DexProto* proto,
                          MethodSearch search,
                          const DexMethod* caller) {
  if (search == MethodSearch::Interface) {
    return resolve_intf_method_ref(cls, name, proto);
  }
  if (search == MethodSearch::Super) {
    if (caller) {
      // caller must be provided. This condition is here to be compatible with
      // old behavior.
      DexType* containing_type = caller->get_class();
      DexClass* containing_class = type_class(containing_type);
      if (containing_class == nullptr) return nullptr;
      DexType* super_class = containing_class->get_super_class();
      if (!super_class) return nullptr;
      cls = type_class(super_class);
    }
    // The rest is the same as virtual.
    search = MethodSearch::Virtual;
  }
  while (cls) {
    if (search == MethodSearch::InterfaceVirtual) {
      auto try_intf = resolve_intf_method_ref(cls, name, proto);
      if (try_intf) {
        return try_intf;
      }
    }
    if (search == MethodSearch::Virtual || search == MethodSearch::Any) {
      for (auto& vmeth : cls->get_vmethods()) {
        if (match(name, proto, vmeth)) {
          return vmeth;
        }
      }
    }
    if (search == MethodSearch::Direct || search == MethodSearch::Static ||
        search == MethodSearch::Any) {
      for (auto& dmeth : cls->get_dmethods()) {
        if (match(name, proto, dmeth)) {
          return dmeth;
        }
      }
    }
    // direct methods only look up the given class
    cls = search != MethodSearch::Direct ? type_class(cls->get_super_class())
                                         : nullptr;
  }
  return nullptr;
}

DexMethod* resolve_method_ref(const DexClass* cls,
                              const DexString* name,
                              const DexProto* proto,
                              MethodSearch search) {
  always_assert(search != MethodSearch::Super);
  if (search != MethodSearch::Interface) {
    const auto& super = cls->get_super_class();
    if (super == nullptr) return nullptr;
    const auto& super_cls = type_class(super);
    auto resolved = resolve_method(super_cls, name, proto, search);
    if (resolved || search != MethodSearch::InterfaceVirtual) {
      return resolved;
    }
  }
  const auto& super_intfs = cls->get_interfaces()->get_type_list();
  for (const auto& super_intf : super_intfs) {
    const auto& super_intf_cls = type_class(super_intf);
    if (super_intf_cls == nullptr) continue;
    auto method = resolve_intf_method_ref(super_intf_cls, name, proto);
    if (method) return method;
  }
  return nullptr;
}

DexField* resolve_field(const DexType* owner,
                        const DexString* name,
                        const DexType* type,
                        FieldSearch fs) {
  auto field_eq = [&](const DexField* a) {
    return a->get_name() == name && a->get_type() == type;
  };

  const DexClass* cls = type_class(owner);
  while (cls) {
    if (fs == FieldSearch::Instance || fs == FieldSearch::Any) {
      for (auto ifield : cls->get_ifields()) {
        if (field_eq(ifield)) {
          return ifield;
        }
      }
    }
    if (fs == FieldSearch::Static || fs == FieldSearch::Any) {
      for (auto sfield : cls->get_sfields()) {
        if (field_eq(sfield)) {
          return sfield;
        }
      }
      // static final fields may be coming from interfaces so we
      // have to walk up the interface hierarchy too
      for (const auto& intf : cls->get_interfaces()->get_type_list()) {
        auto field = resolve_field(intf, name, type, fs);
        if (field != nullptr) return field;
      }
    }
    cls = type_class(cls->get_super_class());
  }
  return nullptr;
}

DexMethod* find_top_impl(const DexClass* cls,
                         const DexString* name,
                         const DexProto* proto) {
  DexMethod* top_impl = nullptr;
  while (cls) {
    for (const auto& vmeth : cls->get_vmethods()) {
      if (match(name, proto, vmeth)) {
        top_impl = vmeth;
      }
    }
    cls = type_class(cls->get_super_class());
  }
  return top_impl;
}

DexMethod* find_top_intf_impl(const DexClass* cls,
                              const DexString* name,
                              const DexProto* proto) {
  DexMethod* top_impl = nullptr;
  while (cls) {
    DexMethod* top_mir_impl = resolve_intf_method_ref(cls, name, proto);
    if (top_mir_impl != nullptr) top_impl = top_mir_impl;
    cls = type_class(cls->get_super_class());
  }
  return top_impl;
}
