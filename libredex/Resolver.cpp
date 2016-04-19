// Copyright 2004-present Facebook. All Rights Reserved.

#include "Resolver.h"
#include "DexUtil.h"

namespace {

inline bool match(const DexString* name,
                  const DexProto* proto,
                  const DexMethod* cls_meth) {
  return name == cls_meth->get_name() && proto == cls_meth->get_proto();
}

DexMethod* check_vmethods(const DexString* name,
                          const DexProto* proto,
                          const DexType* type) {
  const auto cls = type_class(type);
  for (auto& method : cls->get_vmethods()) {
    if (match(name, proto, method)) return method;
  }
  return nullptr;
}

DexMethod* check_dmethods(const DexString* name,
                          const DexProto* proto,
                          const DexType* type) {
  const auto cls = type_class(type);
  for (auto& method : cls->get_dmethods()) {
    if (match(name, proto, method)) return method;
  }
  return nullptr;
}

DexMethod* resolve_intf_methodref(DexType* intf, DexMethod* meth) {

  auto find_method = [&](const DexClass* cls) -> DexMethod* {
    always_assert(cls->get_access() & ACC_INTERFACE);
    const auto& vmethods = cls->get_vmethods();
    for (const auto vmethod : vmethods) {
      if (vmethod->get_name() == meth->get_name() &&
          vmethod->get_proto() == meth->get_proto()) {
        return vmethod;
      }
    }
    return nullptr;
  };

  const auto intf_cls = type_class(intf);
  if (intf_cls == nullptr) return nullptr;
  auto method = find_method(intf_cls);
  if (method) return method;
  const auto& super_intfs = intf_cls->get_interfaces()->get_type_list();
  for (auto super_intf : super_intfs) {
    method = resolve_intf_methodref(super_intf, meth);
    if (method) return method;
  }
  return nullptr;
}

}

DexMethod* resolve_method(
    const DexClass* cls, const DexString* name,
    const DexProto* proto, MethodSearch search) {
  while (cls) {
    if (search == MethodSearch::Virtual || search == MethodSearch::Any) {
      for (auto& vmeth : cls->get_vmethods()) {
        if (match(name, proto, vmeth)) {
          return vmeth;
        }
      }
    }
    if (search == MethodSearch::Direct || search == MethodSearch::Static
        || search == MethodSearch::Any) {
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

DexMethod* find_top_impl(
    const DexClass* cls, const DexString* name, const DexProto* proto) {
  DexMethod* top_impl = nullptr;
  while (cls) {
    for (const auto& vmeth : cls->get_vmethods()) {
      if (match(name, proto, vmeth)) {
        top_impl = vmeth;
      }
    }
    cls = type_class_internal(cls->get_super_class());
  }
  return top_impl;
}

DexMethod* find_collision_excepting(const DexMethod* except,
                                    const DexString* name,
                                    const DexProto* proto,
                                    const DexClass* cls,
                                    bool is_virtual,
                                    bool check_direct) {
  for (auto& method : cls->get_dmethods()) {
    if (match(name, proto, method) && method != except) return method;
  }
  for (auto& method : cls->get_vmethods()) {
    if (match(name, proto, method) && method != except) return method;
  }
  if (!is_virtual) return nullptr;

  auto super = type_class(cls->get_super_class());
  if (super) {
    auto method = resolve_virtual(super, name, proto);
    if (method && method != except) return method;
  }

  TypeVector children;
  get_all_children(cls->get_type(), children);
  for (const auto& child : children) {
    auto vmethod = check_vmethods(name, proto, child);
    if (vmethod && vmethod != except) return vmethod;
    if (check_direct) {
      auto dmethod = check_dmethods(name, proto, child);
      if (dmethod && dmethod != except) return dmethod;
    }
  }
  return nullptr;
}

DexMethod* resolve_intf_methodref(DexMethod* meth) {
  auto method = resolve_intf_methodref(meth->get_class(), meth);
  return method;
}

DexField* resolve_field(
    const DexType* owner,
    const DexString* name,
    const DexType* type,
    FieldSearch fs) {
  auto field_eq =
      [&](const DexField* a) {
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
    }
    cls = type_class(cls->get_super_class());
  }
  return nullptr;
}

