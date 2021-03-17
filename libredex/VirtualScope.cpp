/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VirtualScope.h"
#include "Creators.h"
#include "DexAccess.h"
#include "DexUtil.h"
#include "ReachableClasses.h"
#include "Show.h"
#include "Timer.h"
#include "Trace.h"

#include <map>
#include <set>

namespace {

/**
 * Create a DexClass for Object, which may be missing if no
 * jar files were specified on the command line
 */
void create_object_class() {
  std::vector<DexMethod*> object_methods;

  auto type = type::java_lang_Object();
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
  auto void_object = DexProto::make_proto(type::java_lang_Object(), void_args);
  auto object_bool = DexProto::make_proto(
      type::_boolean(),
      DexTypeList::make_type_list({type::java_lang_Object()}));
  auto void_void = DexProto::make_proto(type::_void(), void_args);
  auto void_class = DexProto::make_proto(type::java_lang_Class(), void_args);
  auto void_int = DexProto::make_proto(type::_int(), void_args);
  auto void_string = DexProto::make_proto(type::java_lang_String(), void_args);
  auto long_void = DexProto::make_proto(
      type::_void(), DexTypeList::make_type_list({type::_int()}));
  auto long_int_void = DexProto::make_proto(
      type::_void(),
      DexTypeList::make_type_list({type::_long(), type::_int()}));

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
  // function is called multiple times with type::java_lang_Object()), we
  // will fail an assertion. This only happens in tests when no external jars
  // are available protected java.lang.Object.clone()Ljava/lang/Object;
  auto method =
      static_cast<DexMethod*>(DexMethod::make_method(type, clone, void_object));
  method->set_access(ACC_PROTECTED);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public java.lang.Object.equals(Ljava/lang/Object;)Z
  method = static_cast<DexMethod*>(
      DexMethod::make_method(type, equals, object_bool));
  method->set_access(ACC_PUBLIC);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // protected java.lang.Object.finalize()V
  method = static_cast<DexMethod*>(
      DexMethod::make_method(type, finalize, void_void));
  method->set_access(ACC_PROTECTED);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public final native java.lang.Object.getClass()Ljava/lang/Class;
  method = static_cast<DexMethod*>(
      DexMethod::make_method(type, getClass, void_class));
  method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public native java.lang.Object.hashCode()I
  method =
      static_cast<DexMethod*>(DexMethod::make_method(type, hashCode, void_int));
  method->set_access(ACC_PUBLIC | ACC_NATIVE);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  method =
      static_cast<DexMethod*>(DexMethod::make_method(type, notify, void_void));
  method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public final native java.lang.Object.notifyAll()V
  method = static_cast<DexMethod*>(
      DexMethod::make_method(type, notifyAll, void_void));
  method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public java.lang.Object.toString()Ljava/lang/String;
  method = static_cast<DexMethod*>(
      DexMethod::make_method(type, toString, void_string));
  method->set_access(ACC_PUBLIC);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public final java.lang.Object.wait()V
  method =
      static_cast<DexMethod*>(DexMethod::make_method(type, wait, void_void));
  method->set_access(ACC_PUBLIC | ACC_FINAL);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public final java.lang.Object.wait(J)V
  method =
      static_cast<DexMethod*>(DexMethod::make_method(type, wait, long_void));
  method->set_access(ACC_PUBLIC | ACC_FINAL);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // public final native java.lang.Object.wait(JI)V
  method = static_cast<DexMethod*>(
      DexMethod::make_method(type, wait, long_int_void));
  method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
  method->set_virtual(true);
  method->set_external();
  object_methods.push_back(method);

  // Now make sure Object itself exists.
  if (type_class(type) == nullptr) {
    ClassCreator cc(type);
    cc.set_access(ACC_PUBLIC);
    auto object_class = cc.create();
    for (auto const& m : object_methods) {
      object_class->add_method(m);
    }
  }
}

// map from a proto to the set of interface implementing that sig
using IntfProtoMap = std::map<const DexProto*, TypeSet, dexprotos_comparator>;

// a map from name to signatures for a set of interfaces
using BaseIntfSigs =
    std::map<const DexString*, IntfProtoMap, dexstrings_comparator>;

// map to track signatures as (name, sig)
using BaseSigs = std::map<const DexString*,
                          std::set<const DexProto*, dexprotos_comparator>,
                          dexstrings_comparator>;

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
  TRACE(VIRT,
        4,
        "merge scopes %s, %s - %s, %s",
        SHOW(scope.type),
        SHOW(scope.methods[0].first),
        SHOW(another.type),
        SHOW(another.methods[0].first));
  for (const auto& meth : another.methods) {
    scope.methods.push_back(meth);
  }
  scope.interfaces.insert(another.interfaces.begin(), another.interfaces.end());
}

/**
 * Deal with the tragic escape story...
 * if an interface down the hierarchy is marked ESCAPED
 * everything defined in base escapes as well.
 */
void escape_all(VirtualScopes& scopes) {
  for (auto& scope : scopes) {
    for (auto& vmeth : scope.methods) {
      TRACE(VIRT, 6, "ESCAPED %s", SHOW(vmeth.first));
      vmeth.second |= ESCAPED;
    }
  }
}

/**
 * Deal with the tragic escape story...
 * if an interface at the class level is marked ESCAPED
 * everything defined in base and children escapes as well.
 */
void escape_all(SignatureMap& sig_map) {
  for (auto& protos_it : sig_map) {
    for (auto& scopes_it : protos_it.second) {
      escape_all(scopes_it.second);
    }
  }
}

/**
 * Mark VirtualFlags at each level walking up the hierarchy.
 * Walk through all the method definitions in base.
 */
void mark_methods(const DexType* type,
                  SignatureMap& sig_map,
                  const BaseSigs& base_sigs,
                  bool escape) {
  for (const auto& protos_it : base_sigs) {
    const auto name = protos_it.first;
    for (const auto& proto : protos_it.second) {
      auto& scopes = sig_map[name][proto];
      always_assert(!scopes.empty());
      always_assert(scopes[0].type == type);
      // mark final and override accordingly
      auto& first_scope = scopes[0];
      if (first_scope.methods.size() == 1) {
        TRACE(VIRT, 6, "FINAL %s", SHOW(first_scope.methods[0].first));
        first_scope.methods[0].second |= FINAL;
      } else {
        for (auto meth = first_scope.methods.begin() + 1;
             meth != first_scope.methods.end();
             meth++) {
          TRACE(VIRT, 6, "OVERRIDE %s", SHOW((*meth).first));
          (*meth).second |= OVERRIDE;
        }
      }
      // all others must be interfaces but we have a definition
      // in base so they must all be override
      if (scopes.size() > 1) {
        for (auto scope = scopes.begin() + 1; scope != scopes.end(); scope++) {
          always_assert(!(*scope).methods.empty());
          TRACE(VIRT, 6, "OVERRIDE %s", SHOW((*scope).methods[0].first));
          (*scope).methods[0].second |= OVERRIDE;
        }
      }
      if (escape) {
        escape_all(scopes);
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
void build_interface_scope(const DexType* type,
                           SignatureMap& sig_map,
                           const BaseIntfSigs& intf_sig_map) {
  for (const auto& proto_it : intf_sig_map) {
    for (const auto& intfs_it : proto_it.second) {
      auto& scopes = sig_map[proto_it.first][intfs_it.first];
      // first virtual scope must be that in type, it's the first we built
      always_assert(scopes[0].type == type);
      // mark impl all the class virtual scope
      for (auto& meth : scopes[0].methods) {
        TRACE(VIRT, 6, "IMPL %s", SHOW(meth.first));
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
 * Merge 2 signatures map. The map from derived_sig_map is copied in
 * base_sig_map.
 * Interface methods in base don't have an entry yet, that will be build later
 * because it's a straight copy of the class virtual scope.
 */
void merge(const BaseSigs& base_sigs,
           const BaseIntfSigs& base_intf_sig_map,
           SignatureMap& base_sig_map,
           const SignatureMap& derived_sig_map) {

  // Helpers

  // is_base_sig(name, proto) - is the (name, proto) a definition in base
  const auto is_base_sig = [&](const DexString* name, const DexProto* proto) {
    TRACE(VIRT, 5, "/check base sigs for %s:%s", SHOW(name), SHOW(proto));
    const auto sigs = base_sigs.find(name);
    if (sigs == base_sigs.end()) return false;
    return sigs->second.count(proto) > 0;
  };

  // is_base_intf_sig(name, proto) - is the (name, proto) an interface in base
  const auto is_base_intf_sig =
      [&](const DexString* name, const DexProto* proto, const DexType* intf) {
        TRACE(VIRT,
              5,
              "/check base intf (%s) sigs for %s:%s",
              SHOW(intf),
              SHOW(name),
              SHOW(proto));
        const auto& sigs = base_intf_sig_map.find(name);
        if (sigs == base_intf_sig_map.end()) return false;
        const auto& intfs_it = sigs->second.find(proto);
        if (intfs_it == sigs->second.end()) return false;
        return intfs_it->second.count(intf) > 0;
      };

  // walk all derived signatures
  for (const auto& derived_sig_entry : derived_sig_map) {
    const auto name = derived_sig_entry.first;
    auto& name_map = base_sig_map[name];
    auto& derived_protos_map = derived_sig_entry.second;
    for (const auto& derived_scopes_it : derived_protos_map) {
      const auto proto = derived_scopes_it.first;
      auto& virt_scopes = name_map[proto];
      // the signature in derived does not exists in base
      if (!is_base_sig(name, proto)) {
        TRACE(VIRT,
              4,
              "- no scope (%s:%s) in base, copy over",
              SHOW(name),
              SHOW(proto));
        // not a known signature in original base, copy over
        const auto& scopes = derived_scopes_it.second;
        virt_scopes.insert(virt_scopes.end(), scopes.begin(), scopes.end());
        if (traceEnabled(VIRT, 4)) {
          for (const auto& scope : scopes) {
            TRACE(VIRT,
                  4,
                  "- copy %s (%s:%s): (%ld) %s",
                  SHOW(scope.type),
                  SHOW(name),
                  SHOW(proto),
                  scope.methods.size(),
                  SHOW(scope.methods[0].first));
          }
        }
        continue;
      }

      // it's a sig (name, proto) in original base, the derived entry
      // needs to merge
      // first scope in base_sig_map must be that of the type under
      // analysis because we built it first and added to the empty vector
      always_assert(!virt_scopes.empty());
      TRACE(VIRT,
            4,
            "- found existing scopes for %s:%s (%ld) - first: %s, %ld, %ld",
            SHOW(name),
            SHOW(proto),
            virt_scopes.size(),
            SHOW(virt_scopes[0].type),
            virt_scopes[0].methods.size(),
            virt_scopes[0].interfaces.size());
      always_assert(virt_scopes[0].type == type::java_lang_Object() ||
                    !is_interface(type_class(virt_scopes[0].type)));
      // walk every scope in derived that we have to merge
      TRACE(VIRT, 4, "-- walking scopes");
      for (const auto& scope : derived_scopes_it.second) {
        // if the scope was for a class (!interface) we merge
        // with that of base which is now the top definition
        TRACE(VIRT,
              4,
              "-- checking scope type %s(%ld)",
              SHOW(scope.type),
              scope.methods.size());
        TRACE(VIRT,
              4,
              "-- is interface 0x%X %d",
              scope.type,
              scope.type != type::java_lang_Object() &&
                  is_interface(type_class(scope.type)));
        if (scope.type == type::java_lang_Object() ||
            !is_interface(type_class(scope.type))) {
          TRACE(VIRT,
                4,
                "-- merging with base scopes %s(%ld) : %s",
                SHOW(virt_scopes[0].type),
                virt_scopes[0].methods.size(),
                SHOW(virt_scopes[0].methods[0].first));
          merge(virt_scopes[0], scope);
          continue;
        }
        // interface case. If derived was for an interface in base
        // do nothing because we will create those entries later
        if (!is_base_intf_sig(name, proto, scope.type)) {
          TRACE(VIRT,
                4,
                "-- unimplemented interface %s:%s - %s, %s",
                SHOW(name),
                SHOW(proto),
                SHOW(scope.type),
                SHOW(scope.methods[0].first));
          virt_scopes.push_back(scope);
          continue;
        }
        TRACE(VIRT,
              4,
              "-- implemented interface %s:%s - %s",
              SHOW(name),
              SHOW(proto),
              SHOW(scope.type));
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
DexMethod* make_miranda(const DexType* type,
                        const DexString* name,
                        const DexProto* proto) {
  auto miranda = DexMethod::make_method(const_cast<DexType*>(type),
                                        const_cast<DexString*>(name),
                                        const_cast<DexProto*>(proto));
  // The next assert may fire because we don't delete DexMethod from the
  // cache and we may find one we have deleted and it was a def.
  // Come up with a better assert story
  // always_assert(!miranda->is_def());
  return static_cast<DexMethod*>(miranda);
}

bool load_interfaces_methods(const std::deque<DexType*>&, BaseIntfSigs&);

/**
 * Load methods for a given interface and its super interfaces.
 * Return true if any interface escapes (no DexClass*).
 */
bool load_interface_methods(const DexClass* intf_cls,
                            BaseIntfSigs& intf_methods) {
  bool escaped = false;
  const auto& interfaces = intf_cls->get_interfaces()->get_type_list();
  if (!interfaces.empty()) {
    if (load_interfaces_methods(interfaces, intf_methods)) {
      escaped = true;
    }
  }
  for (const auto& meth : intf_cls->get_vmethods()) {
    intf_methods[meth->get_name()][meth->get_proto()].insert(
        intf_cls->get_type());
  }
  return escaped;
}

/**
 * Load methods for a list of interfaces.
 * If any interface escapes (no DexClass*) return true.
 */
bool load_interfaces_methods(const std::deque<DexType*>& interfaces,
                             BaseIntfSigs& intf_methods) {
  bool escaped = false;
  for (const auto& intf : interfaces) {
    auto intf_cls = type_class(intf);
    if (intf_cls == nullptr) {
      TRACE(VIRT, 5, "[Unknown interface: %s]", SHOW(intf));
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
bool get_interface_methods(const DexType* type, BaseIntfSigs& intf_methods) {
  always_assert_log(intf_methods.empty(), "intf_methods is an out param");
  // REVIEW: should we always have a DexClass for java.lang.Object?
  if (type == type::java_lang_Object()) return false;
  auto cls = type_class(type);
  always_assert_log(
      cls != nullptr, "DexClass must exist for type %s\n", SHOW(type));
  bool escaped = false;
  const auto& interfaces = cls->get_interfaces()->get_type_list();
  if (!interfaces.empty()) {
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
bool load_interfaces(const DexType* type,
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
      if (sig_map[name][proto].empty()) {
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
      sig_map[name][proto][0].interfaces.insert(proto_it.second.begin(),
                                                proto_it.second.end());
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
bool build_signature_map(const ClassHierarchy& hierarchy,
                         const DexType* type,
                         SignatureMap& sig_map) {
  always_assert_log(sig_map.empty(),
                    "intf_methods and children_methods are out params");
  const TypeSet& children = hierarchy.at(type);
  TRACE(VIRT, 3, "* Visit %s", SHOW(type));

  load_methods(type, sig_map);
  // will hold all the signature introduced by interfaces in type
  BaseIntfSigs intf_sig_map;
  bool escape_down = load_interfaces(type, sig_map, intf_sig_map);
  BaseSigs base_sigs = load_base_sigs(sig_map);
  TRACE(VIRT, 3, "* Sig map computed for %s", SHOW(type));

  // recurse through every child to collect all methods
  // and interface methods under type
  bool escape_up = false;
  for (const auto& child : children) {
    SignatureMap child_sig_map;
    escape_up =
        build_signature_map(hierarchy, child, child_sig_map) || escape_up;
    TRACE(VIRT,
          3,
          "* Merging sig map of %s with child %s",
          SHOW(type),
          SHOW(child));
    merge(base_sigs, intf_sig_map, sig_map, child_sig_map);
  }

  TRACE(VIRT, 3, "* Marking methods at %s", SHOW(type));
  mark_methods(type, sig_map, base_sigs, escape_up);
  build_interface_scope(type, sig_map, intf_sig_map);
  if (escape_down) {
    escape_all(sig_map);
  }

  TRACE(VIRT, 3, "* Visited %s(%d, %d)", SHOW(type), escape_up, escape_down);
  return escape_up | escape_down;
}

const VirtualScope* find_rooted_scope(const SignatureMap& sig_map,
                                      const DexType* type,
                                      const DexMethod* meth) {
  const auto& protos = sig_map.find(meth->get_name());
  always_assert(protos != sig_map.end());
  const auto& scopes = protos->second.find(meth->get_proto());
  always_assert(scopes != protos->second.end());
  for (const auto& scope : scopes->second) {
    if (scope.type == type &&
        method::signatures_match(scope.methods[0].first, meth)) {
      return &scope;
    }
  }
  return nullptr;
}

/**
 * Find all interface scopes rooted at the class provided.
 * Those are the scope rooted at a MIRANDA method as in
 * interface I { void m(); }
 * abstract class A imlements I {}
 * class B entends A { public void m() {} }
 * Class A will have a virtual scope for m().
 */
void get_rooted_interface_scope(const SignatureMap& sig_map,
                                const DexType* type,
                                const DexClass* cls,
                                Scopes& cls_scopes) {
  const auto& intfs = cls->get_interfaces()->get_type_list();
  for (const auto& intf : intfs) {
    const DexClass* intf_cls = type_class(intf);
    if (intf_cls == nullptr) continue;
    for (const auto& meth : intf_cls->get_vmethods()) {
      const auto scope = find_rooted_scope(sig_map, type, meth);
      if (scope != nullptr && scope->type == type &&
          !scope->methods[0].first->is_def()) {
        const auto& existing_scopes = cls_scopes.find(type);
        if (existing_scopes != cls_scopes.end()) {
          bool already_found = false;
          for (const auto& existing_scope : existing_scopes->second) {
            if (existing_scope == scope) {
              already_found = true;
            }
          }
          if (already_found) continue;
        }
        TRACE(VIRT,
              9,
              "add rooted interface scope for %s (%s) on %s",
              show_deobfuscated(meth).c_str(),
              SHOW(meth->get_name()),
              SHOW(type));
        cls_scopes[type].emplace_back(scope);
      }
    }
    get_rooted_interface_scope(sig_map, type, intf_cls, cls_scopes);
  }
}

/**
 * Find all scopes rooted to a given type and adds it to
 * ClassScope for the given type.
 */
void get_root_scopes(const SignatureMap& sig_map,
                     const DexType* type,
                     Scopes& cls_scopes) {
  const std::vector<DexMethod*>& methods = get_vmethods(type);
  TRACE(VIRT, 9, "found %ld vmethods for %s", methods.size(), SHOW(type));
  for (const auto meth : methods) {
    const auto& protos = sig_map.find(meth->get_name());
    always_assert(protos != sig_map.end());
    const auto& scopes = protos->second.find(meth->get_proto());
    always_assert(scopes != protos->second.end());
    for (const auto& scope : scopes->second) {
      if (scope.type == type) {
        TRACE(VIRT, 9, "add virtual scope for %s", SHOW(type));
        always_assert(scope.methods[0].first == meth);
        cls_scopes[type].emplace_back(&scope);
      }
    }
  }
  get_rooted_interface_scope(sig_map, type, type_class(type), cls_scopes);
}

} // namespace

SignatureMap build_signature_map(const ClassHierarchy& class_hierarchy) {
  SignatureMap signature_map;
  build_signature_map(class_hierarchy, type::java_lang_Object(), signature_map);
  return signature_map;
}

const std::vector<DexMethod*>& get_vmethods(const DexType* type) {
  const DexClass* cls = type_class(type);
  if (cls == nullptr) {
    always_assert_log(
        type == type::java_lang_Object(), "Unknown type %s\n", SHOW(type));
    create_object_class();
    cls = type_class(type);
  }
  return cls->get_vmethods();
}

const VirtualScope& find_virtual_scope(const SignatureMap& sig_map,
                                       const DexMethod* meth) {
  const auto& protos = sig_map.find(meth->get_name());
  always_assert(protos != sig_map.end());
  const auto& scopes = protos->second.find(meth->get_proto());
  always_assert(scopes != protos->second.end());
  const auto meth_type = meth->get_class();
  for (const auto& scope : scopes->second) {
    if (scope.type == type::java_lang_Object()) return scope;
    if (type::is_subclass(scope.type, meth_type)) return scope;
  }
  not_reached_log("unreachable. Scope not found for %s\n", SHOW(meth));
}

bool can_rename_scope(const VirtualScope* scope) {
  for (const auto& vmeth : scope->methods) {
    if (!can_rename(vmeth.first) || (vmeth.second & ESCAPED) != 0) {
      return false;
    }
  }
  return true;
}

std::vector<const DexMethod*> select_from(const VirtualScope* scope,
                                          const DexType* type) {
  std::vector<const DexMethod*> refined_scope;
  std::unordered_map<const DexType*, DexMethod*> non_child_methods;
  bool found_root_method = false;
  for (const auto& method : scope->methods) {
    if (type::check_cast(method.first->get_class(), type)) {
      found_root_method =
          found_root_method || type == method.first->get_class();
      refined_scope.emplace_back(method.first);
    } else {
      non_child_methods[method.first->get_class()] = method.first;
    }
  }
  if (!found_root_method) {
    auto cls = type_class(type);
    while (cls != nullptr) {
      const auto super = cls->get_super_class();
      const auto& meth = non_child_methods.find(super);
      if (meth != non_child_methods.end()) {
        refined_scope.emplace_back(meth->second);
        return refined_scope;
      }
      cls = type_class(super);
    }
  }
  return refined_scope;
}

const std::vector<const VirtualScope*> ClassScopes::empty_scope =
    std::vector<const VirtualScope*>();
const std::vector<std::vector<const VirtualScope*>>
    ClassScopes::empty_interface_scope =
        std::vector<std::vector<const VirtualScope*>>();

ClassScopes::ClassScopes(const Scope& scope) {
  m_hierarchy = build_type_hierarchy(scope);
  m_interface_map = build_interface_map(m_hierarchy);
  m_sig_map = build_signature_map(m_hierarchy);
  build_class_scopes(type::java_lang_Object());
  build_interface_scopes();
}

const ClassHierarchy& ClassScopes::get_parent_to_children() const {
  return m_hierarchy;
}

/**
 * Builds the ClassScope for type and children.
 * Calling with type::java_lang_Object() builds the ClassScope
 * for the entire system as redex knows it.
 */
void ClassScopes::build_class_scopes(const DexType* type) {
  auto cls = type_class(type);
  always_assert(cls != nullptr || type == type::java_lang_Object());
  get_root_scopes(m_sig_map, type, m_scopes);

  const auto& children_it = m_hierarchy.find(type);
  if (children_it != m_hierarchy.end()) {
    for (const auto& child : children_it->second) {
      build_class_scopes(child);
    }
  }
}

void ClassScopes::build_interface_scopes() {
  for (const auto& intf_it : m_interface_map) {
    const DexClass* intf_cls = type_class(intf_it.first);
    if (intf_cls == nullptr) {
      TRACE_NO_LINE(VIRT, 9, "missing DexClass for %s", SHOW(intf_it.first));
      continue;
    }
    for (const auto& meth : intf_cls->get_vmethods()) {
      const auto& scopes = m_sig_map[meth->get_name()][meth->get_proto()];
      // at least the method itself
      always_assert_log(!scopes.empty(), "Scope empty for %s", SHOW(meth));
      auto& intf_scope = m_interface_scopes[intf_it.first];
      intf_scope.push_back({});
      for (const auto& scope : scopes) {
        if (scope.interfaces.count(intf_it.first) == 0) continue;
        TRACE(VIRT, 9, "add interface scope for %s", SHOW(intf_it.first));
        intf_scope.back().push_back(&scope);
      }
    }
  }
}

InterfaceScope ClassScopes::find_interface_scope(const DexMethod* meth) const {
  InterfaceScope intf_scope;
  DexType* intf = meth->get_class();
  if (m_sig_map.count(meth->get_name()) == 0 ||
      m_sig_map.at(meth->get_name()).count(meth->get_proto()) == 0) {
    return intf_scope;
  }

  const auto& scopes = m_sig_map.at(meth->get_name()).at(meth->get_proto());
  always_assert(!scopes.empty()); // at least the method itself
  for (const auto& scope : scopes) {
    if (scope.interfaces.count(intf) == 0) continue;
    TRACE_NO_LINE(VIRT, 9, "add interface scope for %s", SHOW(intf));
    intf_scope.push_back(&scope);
  }
  return intf_scope;
}

std::string ClassScopes::show_type(const DexType* type) { return show(type); }
