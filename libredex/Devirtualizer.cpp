/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Devirtualizer.h"
#include "DexUtil.h"
#include "DexAccess.h"
#include "Trace.h"

#include <map>
#include <set>

namespace {

std::vector<DexMethod*> object_methods;
/**
 * Return the list of methods for a given type.
 * If the type is java.lang.Object and it is not known (no DexClass for it)
 * it generates fictional methods for it.
 */
void load_object_vmethods() {
  if (object_methods.size() > 0) return;

  auto type = get_object_type();
  // create the following methods:
  // protected java.lang.Object.clone()Ljava/lang/Object;
  // public java.lang.Object.equals(Ljava/lang/Object;)Z
  // protected java.lang.Object.finalize()V
  // public final native java.lang.Object.getClass()Ljava/lang/Class;
  // public native java.lang.Object.hashCode()I
  // public final native java.lang.Object.notify()V
  // public final native java.lang.Object.notifyAll()V
  // public java.lang.Object.toString()Ljava/lang/String;
  // public final java.lang.Object.wait()V
  // public final java.lang.Object.wait(J)V
  // public final native java.lang.Object.wait(JI)V

  // required sigs
  auto void_args = DexTypeList::make_type_list({});
  auto void_object = DexProto::make_proto(get_object_type(), void_args);
  auto object_bool = DexProto::make_proto(
      get_boolean_type(), DexTypeList::make_type_list({get_object_type()}));
  auto void_void = DexProto::make_proto(get_void_type(), void_args);
  auto void_class = DexProto::make_proto(get_class_type(), void_args);
  auto void_int = DexProto::make_proto(get_int_type(), void_args);
  auto void_string = DexProto::make_proto(get_string_type(), void_args);
  auto long_void = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({get_int_type()}));
  auto long_int_void = DexProto::make_proto(
      get_void_type(),
      DexTypeList::make_type_list({get_long_type(), get_int_type()}));

  // required names
  auto clone = DexString::make_string("clone");
  auto equals = DexString::make_string("equals");
  auto finalize = DexString::make_string("finalize");
  auto getClass = DexString::make_string("getClass");
  auto hashCode = DexString::make_string("hashCode");
  auto notify = DexString::make_string("notify");
  auto notifyAll = DexString::make_string("notifyAll");
  auto toString = DexString::make_string("toString");
  auto wait = DexString::make_string("wait");

  // create methods and add to the list of object methods
  // All the checks to see if the methods exist are because we cannot set
  // access/virtual for external methods, so if the method exists (i.e. if this
  // function is called multiple times with get_object_type()), we will fail
  // an assertion. This only happens in tests when no external jars are
  // available
  // protected java.lang.Object.clone()Ljava/lang/Object;
  DexMethod* method = DexMethod::get_method(type, clone, void_object);
  if (method == nullptr) {
    method = DexMethod::make_method(type, clone, void_object);
    method->set_access(ACC_PROTECTED);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);

  // public java.lang.Object.equals(Ljava/lang/Object;)Z
  method = DexMethod::get_method(type, equals, object_bool);
  if (method == nullptr) {
    method = DexMethod::make_method(type, equals, object_bool);
    method->set_access(ACC_PUBLIC);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, finalize, void_void);
  if (method == nullptr) {
    // protected java.lang.Object.finalize()V
    method = DexMethod::make_method(type, finalize, void_void);
    method->set_access(ACC_PROTECTED);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, getClass, void_class);
  if (method == nullptr) {
    // public final native java.lang.Object.getClass()Ljava/lang/Class;
    method = DexMethod::make_method(type, getClass, void_class);
    method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, hashCode, void_int);
  if (method == nullptr) {
    // public native java.lang.Object.hashCode()I
    method = DexMethod::make_method(type, hashCode, void_int);
    method->set_access(ACC_PUBLIC | ACC_NATIVE);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, notify, void_void);
  if (method == nullptr) {
    // public final native java.lang.Object.notify()V
    method = DexMethod::make_method(type, notify, void_void);
    method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, notifyAll, void_void);
  if (method == nullptr) {
    // public final native java.lang.Object.notifyAll()V
    method = DexMethod::make_method(type, notifyAll, void_void);
    method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, toString, void_string);
  if (method == nullptr) {
    // public java.lang.Object.toString()Ljava/lang/String;
    method = DexMethod::make_method(type, toString, void_string);
    method->set_access(ACC_PUBLIC);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, wait, void_void);
  if (method == nullptr) {
    // public final java.lang.Object.wait()V
    method = DexMethod::make_method(type, wait, void_void);
    method->set_access(ACC_PUBLIC | ACC_FINAL);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, wait, long_void);
  if (method == nullptr) {
    // public final java.lang.Object.wait(J)V
    method = DexMethod::make_method(type, wait, long_void);
    method->set_access(ACC_PUBLIC | ACC_FINAL);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, wait, long_int_void);
  if (method == nullptr) {
    // public final native java.lang.Object.wait(JI)V
    method = DexMethod::make_method(type, wait, long_int_void);
    method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
}

/**
 * Merge the methods map in derived with that of base for the given type.
 */
void merge(SignatureMap& base, SignatureMap& derived) {
  for (const auto& entry : derived) {
    const auto& base_entry = base.find(entry.first);
    if (base_entry == base.end()) {
      base[entry.first] = entry.second;
      continue;
    }
    auto& sig_to_meths = base_entry->second;
    for (const auto& sigs : entry.second) {
      auto& meths = sig_to_meths[sigs.first];
      meths.insert(meths.end(), sigs.second.begin(), sigs.second.end());
    }
  }
}

/**
 * Merge the interface methods map in derived with that of base.
 */
void merge(
    InterfaceSigMap& intf_methods, InterfaceSigMap& child_intf_methods) {
  // TODO: remove after we wrote enough unit tests to make sure all cases
  //       are covered
  // intf_methods.insert(child_intf_methods.begin(), child_intf_methods.end());
  for (const auto& child_meth_it : child_intf_methods) {
    const auto& intf_protos = intf_methods.find(child_meth_it.first);
    if (intf_protos == intf_methods.end()) {
      intf_methods[child_meth_it.first] = child_meth_it.second;
      continue;
    }
    auto& proto_set = intf_protos->second;
    proto_set.insert(child_meth_it.second.begin(), child_meth_it.second.end());
  }
}

//
// Helpers to load interface methods in a MethodMap.
//

bool load_interfaces_methods(const std::deque<DexType*>&, InterfaceSigMap&);

/**
 * Load methods for a given interface and its super interfaces.
 * Return true if any interface escapes (no DexClass*).
 */
bool load_interface_methods(const DexClass* intf_cls,
                            InterfaceSigMap& methods) {
  bool escaped = false;
  const auto& interfaces = intf_cls->get_interfaces()->get_type_list();
  if (interfaces.size() > 0) {
    if (load_interfaces_methods(interfaces, methods)) escaped = true;
  }
  for (const auto& meth : intf_cls->get_vmethods()) {
    methods[meth->get_name()].insert(meth->get_proto());
  }
  return escaped;
}

/**
 * Load methods for a list of interfaces.
 * If any interface escapes (no DexClass*) return true.
 */
bool load_interfaces_methods(
    const std::deque<DexType*>& interfaces, InterfaceSigMap& methods) {
  bool escaped = false;
  for (const auto& intf : interfaces) {
    auto intf_cls = type_class(intf);
    if (intf_cls == nullptr) {
      TRACE(VIRT, 3, "Unknown interface: %s\n", SHOW(intf));
      escaped = true;
      continue;
    }
    if (load_interface_methods(intf_cls, methods)) escaped = true;
  }
  return escaped;
}

/**
 * Get all interface methods for a given type.
 */
bool get_interface_methods(const DexType* type, InterfaceSigMap& methods) {
  always_assert_log(methods.size() == 0, "methods is an out param");
  // REVIEW: should we always have a DexClass for java.lang.Object?
  if (type == get_object_type()) return false;
  auto cls = type_class(type);
  always_assert_log(cls != nullptr, "DexClass must exist for type %s\n", SHOW(type));
  bool escaped = false;
  const auto& interfaces = cls->get_interfaces()->get_type_list();
  if (interfaces.size() > 0) {
    if (load_interfaces_methods(interfaces, methods)) escaped = true;
  }
  return escaped;
}

/**
* Mark all methods in map as 'impl'.
* This happens when an interface on some class is unknown and so we have
* no way to tell if any of the children methods is an implementation of
* that interface. So we conservatively mark every children method 'impl'.
*/
void impl_all(SignatureMap& methods) {
  for (auto& meths_by_name : methods) {
    for (auto& meths_by_proto : meths_by_name.second) {
      for (auto& meth : meths_by_proto.second) {
        meth.second |= IMPL;
      }
    }
  }
}

/**
* Given a set of interface methods from a parent, mark all children methods
* that match as 'impl'.
*/
void impl_intf_methods(
    SignatureMap& methods, InterfaceSigMap& intf_methods) {
  for (const auto& intf_meths_by_name : intf_methods) {
    const auto& meths_by_name = methods.find(intf_meths_by_name.first);
    if (meths_by_name == methods.end()) continue;
    for (const auto& intf_meth_sig : intf_meths_by_name.second) {
      const auto& meths_by_sig = meths_by_name->second.find(intf_meth_sig);
      if (meths_by_sig == meths_by_name->second.end()) continue;
      for (auto& meth : meths_by_sig->second) {
        meth.second |= IMPL;
      }
    }
  }
}

/**
 * Mark all children methods according to parent and children interfaces:
 * - for every method in parent:
 *    - if escape is true mark it 'impl'
 *    - if matches a method in children interfaces mark it 'impl'
 *    - if method not in children methods mark it 'final'
 *    - otherwise leave it as is
 * - for every method in children that matches name and sig of a method
 * in parent, mark it 'override'
 */
void analyze_parent_children_methods(
    const DexType* parent, SignatureMap& children_methods,
    InterfaceSigMap& children_intf_methods, bool escape) {
  auto const& vmethods = get_vmethods(parent);
  for (auto& vmeth : vmethods) {
    auto meth_acc = std::make_pair(vmeth, TOP_DEF);
    if (escape) {
      meth_acc.second |= IMPL;
    } else {
      const auto& intf_meth_by_name =
          children_intf_methods.find(vmeth->get_name());
      if (intf_meth_by_name != children_intf_methods.end() &&
          intf_meth_by_name->second.count(vmeth->get_proto()) > 0) {
        meth_acc.second |= IMPL;
      }
    }
    auto& meths_by_name = children_methods[vmeth->get_name()];
    auto& meths_by_proto = meths_by_name[vmeth->get_proto()];
    if (meths_by_proto.size() == 0) {
      // first time we see this method, make it final if not in escape mode
      meth_acc.second |= FINAL;
    } else {
      // we have seen the method already mark all seen override
      for (auto& meth : meths_by_proto) {
        meth.second |= OVERRIDE;
      }
    }
    // add current method to list of methods for that name and sig
    meths_by_proto.push_back(meth_acc);
  }
}

/**
 * Compute methods FINAL, OVERRIDE and IMPL properties.
 * Starting from java.lang.Object recursively walk the type hierarchy down
 * BFS style and while unwinding compare each method in the class being
 * traversed with all methods coming from the children.
 * Then perform the following:
 * 1- if a method in the parent does not exist in any children mark it FINAL
 * 2- if a method in the parent matches a list of methods in the children
 * mark all children OVERRIDE
 * 3- if a method is an implementation of an interface method mark it IMPL
 *
 * At the end top methods (where the method is introduced) are the only
 * non OVERRIDE and possibly non IMPL.
 * Any method that is FINAL and not OVERRIDE or IMPL is effectively a
 * non virtual.
 * Interfaces add a painful spin to this, best expressed by examples:
 * class A { void m() {} }
 * interface I { void m(); }
 * class B extends A implements I {}
 * in this case A.m() must be marked IMPL even though is up in the hierarchy
 * chain. If not, it would be a FINAL non OVERRIDE and could be inlined and
 * deleted breaking the interface contract. So we mark all methods that
 * match interface down the hierarchy as IMPL.
 * If an interface is not known (escapes) we mark all children methods
 * and all methods up the hierarchy chain IMPL.
 * Consider this example and assume interface I is unknown
 * class A { public m() {} public g() {} public f() {} }
 * class B extends A implements I {}
 * class C extends B { public void k() {} }
 * class D extends A { public void k() {} }
 * in this case, not knowing interface I, we mark all methods in A, B and C
 * IMPL but methods in D are not, so in this case they are just FINAL and
 * effectively D.k() would be non virtual as opposed to C.k() which is IMPL.
 */
bool mark_methods(
    const ClassHierarchy& hierarchy,
    const DexType* type, const TypeSet& children,
    InterfaceSigMap& intf_methods, SignatureMap& methods) {
  always_assert_log(intf_methods.size() == 0 && methods.size() == 0,
      "intf_methods and children_methods are out params");
  bool escape = false;
  // recurse through every child in a BFS style to collect all methods
  // and interface methods under type
  for (const auto& child : children) {
    SignatureMap child_methods;
    InterfaceSigMap child_intf_methods;
    escape = mark_methods(hierarchy, child, hierarchy.at(child),
        child_intf_methods, child_methods) || escape;
    merge(methods, child_methods);
    merge(intf_methods, child_intf_methods);
  }
  // get type interface methods
  InterfaceSigMap type_intf_methods;
  bool escape_intf = get_interface_methods(type, type_intf_methods);
  merge(intf_methods, type_intf_methods);

  escape = escape || escape_intf;

  analyze_parent_children_methods(type, methods, intf_methods, escape);

  if (escape_intf) {
    // if any interface in type escapes, mark all children methods 'impl'
    impl_all(methods);
  } else {
    impl_intf_methods(methods, type_intf_methods);
  }

  return escape;
}

/**
 * Given a class walks up the hierarchy and creates entries from parent to
 * children.
 * If no super is found the type is considered a child of java.lang.Object.
 * If the type is unknown (no DexClass) the walk stops and the hierarchy is
 * formed up to the first unknown type.
 */
void build_class_hierarchy(ClassHierarchy& hierarchy, DexClass* cls) {
  // ensure an entry for the DexClass is created
  hierarchy[cls->get_type()];
  while (cls != nullptr) {
    auto type = cls->get_type();
    const auto super = cls->get_super_class();
    if (super != nullptr) {
      hierarchy[super].insert(type);
    } else {
      if (type != get_object_type()) {
        // if the type in question is not java.lang.Object and it has
        // no super make it a subclass of java.lang.Object
        hierarchy[get_object_type()].insert(type);
        TRACE(SINL, 4, "no super on %s\n", SHOW(type));
      }
    }
    cls = type_class(super);
  }
}

}

ClassHierarchy build_type_hierarchy(const Scope& scope) {
  ClassHierarchy hierarchy;
  // build the type hierarchy
  for (const auto& cls : scope) {
    if (is_interface(cls) || is_annotation(cls)) continue;
    build_class_hierarchy(hierarchy, cls);
  }
  return hierarchy;
}

SignatureMap build_signature_map(const ClassHierarchy& class_hierarchy) {
  // build the signatures map
  auto object = get_object_type();
  const auto& children = class_hierarchy.at(object);
  SignatureMap signature_map;
  InterfaceSigMap intf_methods;
  mark_methods(class_hierarchy, object, children, intf_methods, signature_map);
  return signature_map;
}

const std::vector<DexMethod*>& get_vmethods(const DexType* type) {
  const DexClass* cls = type_class(type);
  if (cls != nullptr) return cls->get_vmethods();
  always_assert_log(type == get_object_type(), "Unknown type %s\n", SHOW(type));
  load_object_vmethods();
  return object_methods;
}
