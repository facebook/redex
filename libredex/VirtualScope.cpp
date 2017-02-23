/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "VirtualScope.h"
#include "DexUtil.h"
#include "DexAccess.h"
#include "Timer.h"
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
  auto method = DexMethod::make_method(type, clone, void_object);
  method->set_access(ACC_PROTECTED);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public java.lang.Object.equals(Ljava/lang/Object;)Z
  method = DexMethod::make_method(type, equals, object_bool);
  method->set_access(ACC_PUBLIC);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // protected java.lang.Object.finalize()V
  method = DexMethod::make_method(type, finalize, void_void);
  method->set_access(ACC_PROTECTED);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public final native java.lang.Object.getClass()Ljava/lang/Class;
  method = DexMethod::make_method(type, getClass, void_class);
  method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public native java.lang.Object.hashCode()I
  method = DexMethod::make_method(type, hashCode, void_int);
  method->set_access(ACC_PUBLIC | ACC_NATIVE);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  method = DexMethod::make_method(type, notify, void_void);
  method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public final native java.lang.Object.notifyAll()V
  method = DexMethod::make_method(type, notifyAll, void_void);
  method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public java.lang.Object.toString()Ljava/lang/String;
  method = DexMethod::make_method(type, toString, void_string);
  method->set_access(ACC_PUBLIC);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public final java.lang.Object.wait()V
  method = DexMethod::make_method(type, wait, void_void);
  method->set_access(ACC_PUBLIC | ACC_FINAL);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public final java.lang.Object.wait(J)V
  method = DexMethod::make_method(type, wait, long_void);
  method->set_access(ACC_PUBLIC | ACC_FINAL);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public final native java.lang.Object.wait(JI)V
  method = DexMethod::make_method(type, wait, long_int_void);
  method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);
}

// map from a proto to the set of interface implementing that sig
using IntfProtoMap = std::map<const DexProto*, TypeSet>;

// a map from name to signatures for a set of interfaces
using BaseIntfSigs = std::map<const DexString*, IntfProtoMap>;

// map to track signatures as (name, sig)
using BaseSigs = std::map<
    const DexString*, std::set<const DexProto*>>;

/**
 * Create a BaseSig which is the set of method definitions in a type.
 */
BaseSigs load_base_sigs(SignatureMap& sig_map) {
  BaseSigs base_sigs;
  for (const auto& proto_it : sig_map) {
    for (const auto& scope : proto_it.second) {
      base_sigs[proto_it.first].insert(scope.first);
    }
  }
  return base_sigs;
}

/**
 * VirtualScope merge functions.
 */
void merge(VirtualScope& scope, const VirtualScope& another) {
  TRACE(VIRT, 3, "merge scopes %s, %s - %s, %s\n",
      SHOW(scope.type), SHOW(scope.methods[0].first),
      SHOW(another.type), SHOW(another.methods[0].first));
  for (const auto& meth : another.methods) {
    scope.methods.push_back(meth);
  }
  scope.interfaces.insert(another.interfaces.begin(), another.interfaces.end());
}

/**
 * Mark VirtualFlags at each level walking up the hierarchy.
 * Walk through all the method definitions in base.
 */
void mark_methods(
    const DexType* type,
    SignatureMap& sig_map,
    const BaseSigs& base_sigs,
    bool escape) {
  for (const auto& protos_it : base_sigs) {
    const auto name = protos_it.first;
    for (const auto& proto : protos_it.second) {
      auto& scopes = sig_map[name][proto];
      always_assert(scopes.size() > 0);
      always_assert(scopes[0].type == type);
      // mark final and override accordingly
      auto& scope = scopes[0];
      if (scope.methods.size() == 1) {
        scope.methods[0].second |= FINAL;
      } else {
        for (auto& meth = ++scopes[0].methods.begin();
            meth != scope.methods.end(); meth++) {
          (*meth).second |= OVERRIDE;
        }
      }
      // all others must be interfaces but we have a definition
      // in base so they must all be override
      if (scopes.size() > 1) {
        for (auto& scope = ++scopes.begin(); scope != scopes.end(); scope++) {
          always_assert((*scope).methods.size() > 0);
          (*scope).methods[0].second |= OVERRIDE;
        }
      }
      // deal with the tragic escape story
      // if an interface down the hierarchy is marked ESCAPED
      // everything defined in base escapes as well
      if (escape) {
        for (auto& scope : scopes) {
          for (auto& vmeth : scope.methods) {
            vmeth.second |= ESCAPED;
          }
        }
      }
    }
  }
}

/**
 * Once the sig map is complete for a type, build the interface scopes
 * for that type. So in
 * interface I { void m(); }
 * class A implements I { void m() {} }
 * this step would build the entry for I.m() adding all the methods
 * in the VirtualScope for A.m().
 */
void build_interface_scope(
    const DexType* type,
    SignatureMap& sig_map,
    const BaseIntfSigs& intf_sig_map) {
  for (const auto& proto_it : intf_sig_map) {
    for (const auto& intfs_it : proto_it.second) {
      auto& scopes = sig_map[proto_it.first][intfs_it.first];
      // first virtual scope must be that in type, it's the first we built
      always_assert(scopes[0].type == type);
      // mark impl all the class virtual scope
      for (auto& meth : scopes[0].methods) {
        meth.second |= IMPL;
      }
      // remaining scopes must be for interfaces so they are
      // marked IMPL already.
      // Scope for interfaces in base are not there yet so
      // make a copy of the class virtual scope for every
      // interface scope
      const auto& intfs = intfs_it.second;
      for (const auto& intf : intfs) {
        VirtualScope vg;
        vg.type = intf;
        vg.methods = scopes[0].methods;
        scopes.push_back(vg);
      }
    }
  }
}

/**
 * Merge 2 signatures map. The map from derived_sig_map is copied in base_sig_map.
 * Interface methods in base don't have an entry yet, that will be build later
 * because it's a straight copy of the class virtual scope.
 */
void merge(
    const BaseSigs& base_sigs,
    const BaseIntfSigs& base_intf_sig_map,
    SignatureMap& base_sig_map,
    const SignatureMap& derived_sig_map) {

  // Helpers

  // is_base_sig(name, proto) - is the (name, proto) a definition in base
  const auto is_base_sig =
      [&](const DexString* name, const DexProto* proto) {
        TRACE(VIRT, 4, "/check base sigs for %s:%s\n",
          SHOW(name), SHOW(proto));
        const auto sigs = base_sigs.find(name);
        if (sigs == base_sigs.end()) return false;
        return sigs->second.count(proto) > 0;
      };

  // is_base_intf_sig(name, proto) - is the (name, proto) an interface in base
  const auto is_base_intf_sig =
      [&](const DexString* name, const DexProto* proto, const DexType* intf) {
        TRACE(VIRT, 4, "/check base intf (%s) sigs for %s:%s\n",
            SHOW(intf), SHOW(name), SHOW(proto));
        const auto& sigs = base_intf_sig_map.find(name);
        if (sigs == base_intf_sig_map.end()) return false;
        const auto& intfs_it = sigs->second.find(proto);
        if (intfs_it == sigs->second.end()) return false;
        return intfs_it->second.count(intf) > 0;
      };

  // walk all derived signatures
  for (const auto& derived_sig_entry : derived_sig_map) {
    const auto name = derived_sig_entry.first;
    auto& derived_protos_map = derived_sig_entry.second;
    for (const auto& derived_scopes_it : derived_protos_map) {
      const auto proto = derived_scopes_it.first;
      // the signature in derived does not exists in base
      if (!is_base_sig(name, proto)) {
        TRACE(VIRT, 3, "- no scope (%s:%s) in base, copy over\n",
            SHOW(name), SHOW(proto));
        // not a known signature in original base, copy over
        for (const auto& scope : derived_scopes_it.second) {
          TRACE(VIRT, 3, "- copy %s (%s:%s): (%ld) %s\n",
              SHOW(scope.type), SHOW(name), SHOW(proto),
              scope.methods.size(),
              SHOW(scope.methods[0].first));
          base_sig_map[name][proto].push_back(scope);
        }
        continue;
      }

      // it's a sig (name, proto) in original base, the derived entry
      // needs to merge
      // first scope in base_sig_map must be that of the type under
      // analysis because we built it first and added to the empty vector
      always_assert(base_sig_map[name][proto].size() > 0);
      TRACE(VIRT, 3,
          "- found existing scopes for %s:%s (%ld) - first: %s, %ld, %ld\n",
          SHOW(name), SHOW(proto), base_sig_map[name][proto].size(),
          SHOW(base_sig_map[name][proto][0].type),
          base_sig_map[name][proto][0].methods.size(),
          base_sig_map[name][proto][0].interfaces.size());
      always_assert(base_sig_map[name][proto][0].type == get_object_type() ||
          !is_interface(type_class(base_sig_map[name][proto][0].type)));
      // walk every scope in derived that we have to merge
      TRACE(VIRT, 3, "-- walking scopes\n");
      for (const auto& scope : derived_scopes_it.second) {
        // if the scope was for a class (!interface) we merge
        // with that of base which is now the top definition
        TRACE(VIRT, 3, "-- checking scope type %s(%ld)\n",
            SHOW(scope.type), scope.methods.size());
        TRACE(VIRT, 3, "-- is interface 0x%X %d\n", scope.type,
            scope.type != get_object_type() && is_interface(type_class(scope.type)));
        if (scope.type == get_object_type() ||
            !is_interface(type_class(scope.type))) {
          TRACE(VIRT, 3, "-- merging with base scopes %s(%ld) : %s\n",
              SHOW(base_sig_map[name][proto][0].type),
              base_sig_map[name][proto][0].methods.size(),
              SHOW(base_sig_map[name][proto][0].methods[0].first));
          merge(base_sig_map[name][proto][0], scope);
          continue;
        }
        // interface case. If derived was for an interface in base
        // do nothing because we will create those entries later
        if (!is_base_intf_sig(name, proto, scope.type)) {
          TRACE(VIRT, 3, "-- unimplemented interface %s:%s - %s, %s\n",
              SHOW(name), SHOW(proto),
              SHOW(scope.type), SHOW(scope.methods[0].first));
          base_sig_map[name][proto].push_back(scope);
          continue;
        }
        TRACE(VIRT, 3, "-- implemented interface %s:%s - %s\n",
            SHOW(name), SHOW(proto), SHOW(scope.type));
      }
    }
  }
}

//
// Helpers to load interface methods in a MethodMap.
//

/**
 * Make an entry for a Miranda method.
 * The ref may not exist yet and we will create it with make_method.
 * That is not causing issues because we are not changing that ref ever.
 */
DexMethod* make_miranda(
    const DexType* type, const DexString* name, const DexProto* proto) {
  auto miranda = DexMethod::make_method(const_cast<DexType*>(type),
      const_cast<DexString*>(name), const_cast<DexProto*>(proto));
  always_assert(!miranda->is_def());
  return miranda;
}

bool load_interfaces_methods(
    const std::deque<DexType*>&, BaseIntfSigs&);

/**
 * Load methods for a given interface and its super interfaces.
 * Return true if any interface escapes (no DexClass*).
 */
bool load_interface_methods(
    const DexClass* intf_cls, BaseIntfSigs& intf_methods) {
  bool escaped = false;
  const auto& interfaces = intf_cls->get_interfaces()->get_type_list();
  if (interfaces.size() > 0) {
    if (load_interfaces_methods(interfaces, intf_methods)) {
      escaped = true;
    }
  }
  for (const auto& meth : intf_cls->get_vmethods()) {
    intf_methods[meth->get_name()][meth->get_proto()].insert(intf_cls->get_type());
  }
  return escaped;
}

/**
 * Load methods for a list of interfaces.
 * If any interface escapes (no DexClass*) return true.
 */
bool load_interfaces_methods(
    const std::deque<DexType*>& interfaces,
    BaseIntfSigs& intf_methods) {
  bool escaped = false;
  for (const auto& intf : interfaces) {
    auto intf_cls = type_class(intf);
    if (intf_cls == nullptr) {
      TRACE(VIRT, 4, "[Unknown interface: %s]\n", SHOW(intf));
      escaped = true;
      continue;
    }
    if (load_interface_methods(intf_cls, intf_methods)) {
      escaped = true;
    }
  }
  return escaped;
}

/**
 * Get all interface methods for a given type.
 */
bool get_interface_methods(
    const DexType* type, BaseIntfSigs& intf_methods) {
  always_assert_log(intf_methods.size() == 0, "intf_methods is an out param");
  // REVIEW: should we always have a DexClass for java.lang.Object?
  if (type == get_object_type()) return false;
  auto cls = type_class(type);
  always_assert_log(
      cls != nullptr, "DexClass must exist for type %s\n", SHOW(type));
  bool escaped = false;
  const auto& interfaces = cls->get_interfaces()->get_type_list();
  if (interfaces.size() > 0) {
    if (load_interfaces_methods(interfaces, intf_methods)) escaped = true;
  }
  return escaped;
}

/**
 * Make sure all the intereface methods are added to the SignatureMap.
 * SignatureMap in input contains only scopes for virtual in the class.
 * After this step a type is fully specified with all its virtual methods
 * and all interface methods that did not have an implementation created
 * (as "pure miranda" methods).
 * interface I { void m(); }
 * abstract class A implements I {}
 * in this case we create an entry for A.m() and mark it miranda
 * even though the method did not exist. It will not be a def (!is_def()).
 */
bool load_interfaces(
    const DexType* type,
    SignatureMap& sig_map,
    BaseIntfSigs& intf_sig_map) {
  bool escaped = get_interface_methods(type, intf_sig_map);
  const auto intf_flags = MIRANDA | IMPL;
  // sig_map contains only the virtual methods in the class and
  // intf_sig_map only the methods in the interface.
  // For any missing methods in the class we create a new (miranda) method.
  // If the method is there already we mark it miranda.
  for (const auto& sig_it : intf_sig_map) {
    for (const auto& proto_it : sig_it.second) {
      const auto& name = sig_it.first;
      const auto& proto = proto_it.first;
      always_assert(sig_map[name][proto].size() <= 1);
      if (sig_map[name][proto].size() == 0) {
        // the method interface is not implemented in current
        // type. The class is abstract or a definition up the
        // hierarchy is present.
        // Make a pure miranda entry
        auto mir_meth = make_miranda(type, name, proto);
        VirtualScope scope;
        scope.type = type;
        scope.methods.emplace_back(mir_meth, intf_flags);
        sig_map[name][proto].push_back(scope);
      } else {
        // the method interface is implemented in the current
        // type, mark it miranda
        auto& scope = sig_map[name][proto][0];
        always_assert(scope.methods.size() == 1);
        scope.methods[0].second |= intf_flags;
      }
      // add the implemented interfaces to the class
      // virtual scope
      sig_map[name][proto][0].interfaces.insert(
          proto_it.second.begin(), proto_it.second.end());
    }
  }
  return escaped;
}

/**
 * Load all virtual methods in the given type and build an entry
 * in the signature map.
 * Those should be the only entries in the SignatureMap in input.
 * They are all TOP_DEF until a parent proves otherwise.
 */
void load_methods(const DexType* type, SignatureMap& sig_map) {
  auto const& vmethods = get_vmethods(type);
  // add each virtual method to the SignatureMap
  for (auto& vmeth : vmethods) {
    auto& proto_map = sig_map[vmeth->get_name()];
    auto& scopes = proto_map[vmeth->get_proto()];
    always_assert(scopes.empty());
    VirtualScope scope;
    scope.type = type;
    scope.methods.emplace_back(vmeth, TOP_DEF);
    scopes.emplace_back(scope);
  }
}

/**
 * Compute VirtualScopes and virtual method flags.
 * Starting from java.lang.Object recursively walk the type hierarchy down
 * and while unwinding compare each method in the class being traversed with
 * all methods coming from the children.
 * Then perform the following:
 * 1- if a method in the parent does not exist in any children mark it FINAL
 * 2- if a method in the parent matches a list of methods in the children
 * mark all children OVERRIDE
 * 3- if a method is an implementation of an interface method mark it IMPL
 * 4- if any escape occurs (unknown interface) mark all method in the branch
 * (up to object as ESCAPED).
 * 5- mark MIRANDA any method that implements an interface at the
 * 'implements' point
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
 * and all methods up the hierarchy chain ESCAPED.
 * Consider this example and assume interface I is unknown
 * class A { public m() {} public g() {} public f() {} }
 * class B extends A implements I {}
 * class C extends B { public void k() {} }
 * class D extends A { public void k() {} }
 * in this case, not knowing interface I, we mark all methods in A, B and C
 * ESCAPED but methods in D are not, so in this case they are just FINAL and
 * effectively D.k() would be non virtual as opposed to C.k() which is ESCAPED.
 */
bool build_signature_map(
    const ClassHierarchy& hierarchy,
    const DexType* type,
    SignatureMap& sig_map) {
  always_assert_log(sig_map.size() == 0,
      "intf_methods and children_methods are out params");
  const TypeSet& children = hierarchy.at(type);
  TRACE(VIRT, 2, "* Visit %s\n", SHOW(type));

  load_methods(type, sig_map);
  // will hold all the signature introduced by interfaces in type
  BaseIntfSigs intf_sig_map;
  bool escape = load_interfaces(type, sig_map, intf_sig_map);
  BaseSigs base_sigs = load_base_sigs(sig_map);
  TRACE(VIRT, 2, "* Sig map computed for %s\n", SHOW(type));

  // recurse through every child to collect all methods
  // and interface methods under type
  for (const auto& child : children) {
    SignatureMap child_sig_map;
    escape = build_signature_map(
        hierarchy,
        child,
        child_sig_map) || escape;
    TRACE(VIRT, 2, "* Merging sig map of %s with child %s\n",
        SHOW(type), SHOW(child));
    merge(base_sigs, intf_sig_map, sig_map, child_sig_map);
  }

  TRACE(VIRT, 2, "* Marking methods at %s\n", SHOW(type));
  mark_methods(type, sig_map, base_sigs, escape);
  build_interface_scope(type, sig_map, intf_sig_map);

  TRACE(VIRT, 2, "* Visited %s(%d)\n", SHOW(type), escape);
  return escape;
}

/**
 * Given a class, walks up the hierarchy and creates entries from parent to
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
        TRACE(VIRT, 4, "[no super on %s]\n", SHOW(type));
      }
    }
    cls = type_class(super);
  }
}

}

ClassHierarchy build_type_hierarchy(const Scope& scope) {
  Timer("Class Hierarchy");
  ClassHierarchy hierarchy;
  // build the type hierarchy
  for (const auto& cls : scope) {
    if (is_interface(cls) || is_annotation(cls)) continue;
    build_class_hierarchy(hierarchy, cls);
  }
  return hierarchy;
}

SignatureMap build_signature_map(const ClassHierarchy& class_hierarchy) {
  Timer("Signature Map");
  SignatureMap signature_map;
  build_signature_map(class_hierarchy, get_object_type(), signature_map);
  return signature_map;
}

const std::vector<DexMethod*>& get_vmethods(const DexType* type) {
  const DexClass* cls = type_class(type);
  if (cls != nullptr) return cls->get_vmethods();
  always_assert_log(type == get_object_type(), "Unknown type %s\n", SHOW(type));
  load_object_vmethods();
  return object_methods;
}
