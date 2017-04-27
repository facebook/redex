/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "VirtualRenamer.h"
#include "DexClass.h"
#include "VirtualScope.h"
#include "DexUtil.h"
#include "DexAccess.h"
#include "Resolver.h"
#include "Trace.h"
#include "Walkers.h"

#include <map>
#include <set>

namespace {

// keep a map from defs to all refs resoving to that def
using RefsMap = std::map<DexMethod*, std::set<DexMethod*>>;

struct VirtualRenamer {
  const ClassHierarchy& cls_hierarchy;
  const SignatureMap& sig_map;
  const ClassScopes& class_scopes;
  const RefsMap& def_refs;

  VirtualRenamer(
    const ClassHierarchy& cls_hierarchy,
    const SignatureMap& sig_map,
    const ClassScopes& class_scopes,
    const RefsMap& def_refs) :
        cls_hierarchy(cls_hierarchy),
        sig_map(sig_map),
        class_scopes(class_scopes),
        def_refs(def_refs) {}
};

/**
 * Debug utility to collect info about ClassScopes.
 * Likely to disappear soon....
 */
void scope_info(
    const ClassScopes& class_scopes,
    const ClassHierarchy& cls_hierarchy) {
  std::map<int, int> easy_scopes;
  std::map<int, int> impl_scopes;
  std::map<int, int> cant_rename_scopes;

  walk_virtual_scopes(class_scopes, get_object_type(), cls_hierarchy,
      [&](const DexType* type, const VirtualScope* scope) {
        const auto cls = type_class(type);
        if (cls == nullptr || cls->is_external()) return;
        auto scope_meth_count = scope->methods.size();
        if (scope_meth_count > 100) {
          TRACE(OBFUSCATE, 2, "BIG SCOPE: %ld on %s\n",
              scope_meth_count, SHOW(scope->methods[0].first));
        }
        // class is internal
        if (!can_rename_scope(scope)) {
          cant_rename_scopes[scope_meth_count]++;
          return;
        }
        if (is_impl_scope(scope)) {
          impl_scopes[scope_meth_count]++;
          return;
        }
        easy_scopes[scope_meth_count]++;
      });

  const auto scope_count =
      [](std::map<int, int> map) {
        size_t c = 0;
        for (const auto& it : map) {
          c += it.second;
        }
        return c;
      };
  const auto method_count =
      [](std::map<int, int> map) {
        size_t c = 0;
        for (const auto& it : map) {
          c += (it .first * it.second);
        }
        return c;
      };
  TRACE(OBFUSCATE, 2,
      "scopes (scope count, method count)\n"
      "easy (%ld, %ld), "
      "impl (%ld, %ld), "
      "can't rename (%ld, %ld)\n",
      scope_count(easy_scopes), method_count(easy_scopes),
      scope_count(impl_scopes), method_count(impl_scopes),
      scope_count(cant_rename_scopes), method_count(cant_rename_scopes));

  const auto by_meth_count =
      [](const char * msg, std::map<int, int> map) {
        TRACE(OBFUSCATE, 2, "%s\n", msg);
        for (auto it = map.rbegin(); it != map.rend(); ++it) {
          TRACE(OBFUSCATE, 2, "%ld <= %ld\n", it->first, it->second);
        }
      };
  TRACE(OBFUSCATE, 2,
      "method count in scope <= scope count with that method count\n");
  by_meth_count("EasyScopes:", easy_scopes);
  by_meth_count("ImplScopes:", impl_scopes);
  by_meth_count("CantRenameScopes:", cant_rename_scopes);
}


//
// Virtual Scopes
//

// Uncomment and use this as a prefix for virtual method
// names for debugging
//const std::string prefix = __Redex__";
const std::string prefix = "";

DexString* get_name(int seed) {
  std::string name = prefix;

  const auto append = [&](int value) {
    always_assert_log(value >= 0 && value < 52,
        "value = %d, seed = %d", value, seed);
    if (value < 26) {
      name += ('A' + value);
    } else {
      name += ('a' + value - 26);
    }
  };

  while (seed >= 52) {
    append(seed % 52);
    seed = (seed / 52) - 1;
  }
  append(seed);
  return DexString::make_string(name);
}

/**
 * A name is usable if it does not collide with an existing
 * one in the def and ref space.
 */
bool usable_name(
    DexString* name,
    const VirtualRenamer& vr,
    const VirtualScope* scope) {
  for (const auto& meth : scope->methods) {
    if (DexMethod::get_method(
        meth.first->get_class(),
        name,
        meth.first->get_proto()) != nullptr) {
      return false;
    }
    assert(scope->methods.size() > 0);
    const auto& refs = vr.def_refs.find(scope->methods[0].first);
    if (refs != vr.def_refs.end()) {
      for (const auto& ref : refs->second) {
        if (DexMethod::get_method(
            ref->get_class(),
            name,
            ref->get_proto()) != nullptr) {
          return false;
        }
      }
    }
  }
  return true;
}

/*
 * Operate on 'seed' and find a name for scope that does not
 * lead to any collision for all defs or refs.
 * * Update 'seed' *
 */
DexString* get_unescaped_name(
    const VirtualRenamer& vr,
    const VirtualScope* scope,
    int& seed) {
  auto name = get_name(seed++);
  while (!usable_name(name, vr, scope)) {
    name = get_name(seed++);
  }
  return name;
}

/*
 * Operate on 'seed' and find a name for all scopes that does not
 * lead to any collision for all defs or refs.
 * * Update 'seed' *
 */
DexString* get_unescaped_name(
    const VirtualRenamer& vr,
    std::vector<const VirtualScope*> scopes,
    int& seed) {
  while (true) {
    auto name = get_name(seed++);
    for (const auto& scope : scopes) {
      if (!usable_name(name, vr, scope)) goto next_name;
    }
    return name;
  next_name: ;
  }
}

/**
 * Rename a given method with the given name.
 */
void rename(DexMethod* meth, DexString* name) {
  //assert(meth->is_concrete() && !meth->is_external());
  DexMethodRef ref;
  ref.cls = meth->get_class();
  ref.name = name;
  ref.proto = meth->get_proto();
  if (meth->get_deobfuscated_name().empty()) {
    meth->set_deobfuscated_name(meth->get_name()->c_str());
  }
  meth->change(ref);
}

/**
 * Rename all refs to the given method.
 */
int rename_scope_ref(
    DexMethod* meth,
    const RefsMap& def_refs,
    DexString* name) {
  int renamed = 0;
  const auto& refs = def_refs.find(meth);
  if (refs == def_refs.end()) return renamed;
  for (auto& ref : refs->second) {
    rename(ref, name);
  }
  return renamed;
}

/**
 * Rename an entire virtual scope.
 */
int rename_scope(
    const VirtualScope* scope,
    const RefsMap& def_refs,
    DexString* name) {
  int renamed = 0;
  for (auto& vmeth : scope->methods) {
    rename(vmeth.first, name);
    if (vmeth.first->is_concrete()) renamed++;
    else {
      TRACE(OBFUSCATE, 2, "not concrete %s\n", SHOW(vmeth.first));
    }
  }
  assert(scope->methods.size() > 0);
  rename_scope_ref(scope->methods[0].first, def_refs, name);
  return renamed;
}

/**
 * Find a method with the given (name, proto) in a class.
 */
DexMethod* find_method(
    const DexClass* cls,
    DexMethod* meth,
    DexString* original_name) {
  for (auto& vmeth : cls->get_vmethods()) {
    if (vmeth->get_name() == original_name &&
        vmeth->get_proto() == meth->get_proto()) {
      return vmeth;
    }
  }
  return (DexMethod*)nullptr;
};

/**
 * Get all virtual scopes and all interfaces implemented
 * for a given signature.
 * Perform no operation if any of the scope involved is
 * !can_rename().
 */
std::vector<const VirtualScope*> get_all_intf_scopes(
    const SignatureMap& sig_map,
    const VirtualScope* scope,
    TypeSet& intfs) {
  std::vector<const VirtualScope*> scopes;
  assert(scope->methods.size() > 0);
  const auto& name = scope->methods[0].first->get_name();
  const auto& proto = scope->methods[0].first->get_proto();
  const auto& all_scopes = sig_map.at(name).at(proto);
  // if any scope cannot be renamed let it go, we don't
  // rename anything
  TypeSet all_intfs;
  for (auto& sc : all_scopes) {
    assert(type_class(sc.type) != nullptr);
    if (type_class(sc.type)->is_external()) return scopes;
    if (!can_rename_scope(&sc)) return scopes;
    all_intfs.insert(sc.interfaces.begin(), sc.interfaces.end());
  }
  // if any interface method that we are about to rename
  // cannot be renamed give up
  for (const auto& intf : all_intfs) {
    assert(type_class(intf) != nullptr);
    const auto meth = find_method(type_class(intf),
        scope->methods[0].first, scope->methods[0].first->get_name());
    assert(meth != nullptr);
    if (!can_rename(meth)) return scopes;
  }

  for (auto& sc : all_scopes) {
    // if it's a virtual scope that does not
    // contribute to any interface scope, don't
    // do anything because we will pick it up in
    // our class hierarchy walk
    if (sc.interfaces.empty()) continue;
    // virtual scope mixed with interface scope
    intfs.insert(sc.interfaces.begin(), sc.interfaces.end());
    scopes.emplace_back(&sc);
  }
  return scopes;
}

/**
 * For a given interface signature (name, proto) rename all
 * interface scopes that signature is for.
 * So, if we had something like
 * interface I1 { void m(); }
 * interface I2 { void m(); }
 * class A implements I1, I2 { void m() {} }
 * we rename I1.m(), I2.m() and A.m() together
 */
int rename_interfaces_scopes(
    const VirtualRenamer& vr,
    const std::vector<const VirtualScope*>& scopes,
    std::unordered_set<const VirtualScope*>& visited,
    int& seed) {
  int renamed = 0;
  for (auto& scope : scopes) {
    if (!can_rename_scope(scope)) continue;
    if (!is_impl_scope(scope)) continue;
    if (visited.count(scope) > 0) continue; // we already processed it
    // interface scopes only

    // get all virtual scopes that have an interface implementation and
    // rename all of them. Also get all interfaces that have that signature
    TypeSet intfs;
    std::vector<const VirtualScope*> concrete_scopes =
        get_all_intf_scopes(vr.sig_map, scope, intfs);
    visited.insert(concrete_scopes.begin(), concrete_scopes.end());
    auto original_name = scope->methods[0].first->get_name();
    auto name =  get_unescaped_name(vr, concrete_scopes, seed);
    for (const auto& conc_scope : concrete_scopes) {
      renamed += rename_scope(conc_scope, vr.def_refs, name);
    }

    // rename interface method only
    for (const auto& intf : intfs) {
      auto intf_cls = type_class(intf);
      assert(intf_cls != nullptr);
      auto intf_meth =
          find_method(intf_cls, scope->methods[0].first, original_name);
      always_assert_log(intf_meth != nullptr,
          "cannot find interface method for %s",
          SHOW(scope->methods[0].first));
      rename(intf_meth, name);
      rename_scope_ref(intf_meth, vr.def_refs, name);
      renamed++;
    }
  }
  return renamed;
}

/**
 * Walk the class hierarchy and for every interface scope apply
 * proper renaming.
 */
int rename_interface_scopes(
    const VirtualRenamer& vr,
    const DexType* type,
    std::unordered_set<const VirtualScope*>& visited,
    int& seed) {
  int renamed = 0;
  const auto cls = type_class(type);
  if (cls != nullptr && !cls->is_external()) {
    const auto& scopes_it = vr.class_scopes.find(type);
    if (scopes_it != vr.class_scopes.end()) {
      renamed = rename_interfaces_scopes(vr, scopes_it->second, visited, seed);
    }
  }

  for (const auto& child : vr.cls_hierarchy.at(type)) {
    renamed += rename_interface_scopes(vr, child, visited, seed);
  }
  return renamed;
}

/**
 * Rename only scopes that are not interface and can_rename.
 */
int rename_virtual_scopes(
    const VirtualRenamer& vr,
    const DexType* type,
    int& seed) {
  int renamed = 0;
  const auto cls = type_class(type);
  // object or external classes are not renamable, move
  // to the children
  if (cls != nullptr && !cls->is_external()) {
    const auto& scopes_it = vr.class_scopes.find(type);
    if (scopes_it != vr.class_scopes.end()) {
      // rename all scopes at this level that are not interface
      // and can be renamed
      for (auto& scope : scopes_it->second) {
        if (!can_rename_scope(scope)) continue;
        if (is_impl_scope(scope)) continue;
        auto name =  get_unescaped_name(vr, scope, seed);
        renamed += rename_scope(scope, vr.def_refs, name);
      }
    }
  }

  always_assert_log(
      vr.cls_hierarchy.find(type) != vr.cls_hierarchy.end(),
      "no entry in ClassHierarchy for type %s\n", SHOW(type));
  // will be used for interface renaming, effectively this
  // gets the last name (seed) for all virtual scopes and
  // interface are treated as all being at the same scope,
  // that is, they will all have different names irrespective
  // of where they are in the hierarchy
  int max_seed = seed;
  // recurse into children
  for (const auto& child : vr.cls_hierarchy.at(type)) {
    int base_seed = seed;
    renamed += rename_virtual_scopes(vr, child, base_seed);
    max_seed = std::max(max_seed, base_seed);
  }
  seed = max_seed;
  return renamed;
}

/**
 * Collect all method refs to concrete methods (definitions).
 */
void collect_refs(Scope& scope, RefsMap& def_refs) {
  walk_opcodes(scope, [](DexMethod*) { return true; },
    [&](DexMethod*, IRInstruction* insn) {
      if (!insn->has_method()) return;
      auto callee = insn->get_method();
      if (callee->is_concrete()) return;
      auto cls = type_class(callee->get_class());
      if (cls == nullptr || cls->is_external()) return;
      DexMethod* top = nullptr;
      if (is_interface(cls)) {
        top = resolve_method(callee, MethodSearch::Interface);
      } else {
        top = find_top_impl(cls, callee->get_name(), callee->get_proto());
      }
      if (top == nullptr || top == callee) return;
      assert(type_class(top->get_class()) != nullptr);
      if (type_class(top->get_class())->is_external()) return;
      // it's a top definition on an internal class, save it
      def_refs[top].insert(callee);
    });
}

}

/**
 * Rename virtual methods.
 */
size_t rename_virtuals(Scope& classes) {
  // build a ClassScope
  const ClassHierarchy cls_hierarchy = build_type_hierarchy(classes);
  const SignatureMap sig_map = build_signature_map(cls_hierarchy);
  ClassScopes class_scopes = get_class_scopes(cls_hierarchy, sig_map);
  scope_info(class_scopes, cls_hierarchy);
  RefsMap def_refs;
  collect_refs(classes, def_refs);
  VirtualRenamer vr(cls_hierarchy, sig_map, class_scopes, def_refs);

  // rename virtual only first
  const auto obj_t = get_object_type();
  int seed = 0;
  size_t renamed = rename_virtual_scopes(vr, obj_t, seed);

  // rename interfaces
  std::unordered_set<const VirtualScope*> visited;
  renamed += rename_interface_scopes(vr, obj_t, visited, seed);
  TRACE(OBFUSCATE, 2, "MAX seed: %d\n", seed);
  return renamed;
  return 0;
}
