/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveUninstantiablesPass.h"

#include <cinttypes>

#include "MethodFixup.h"
#include "RemoveUninstantiablesImpl.h"
#include "Walkers.h"

namespace {

struct VirtualScopeId {
  const DexString* name;
  DexProto* proto;
  static VirtualScopeId make(DexMethodRef* method) {
    return VirtualScopeId{method->get_name(), method->get_proto()};
  }
};

struct VirtualScopeIdHasher {
  size_t operator()(const VirtualScopeId& vs) const {
    return ((size_t)vs.name) * 27 + (size_t)vs.proto;
  }
};

bool operator==(const VirtualScopeId& a, const VirtualScopeId& b) {
  return a.name == b.name && a.proto == b.proto;
}

using VirtualScopeIdSet =
    std::unordered_set<VirtualScopeId, VirtualScopeIdHasher>;

// Helper analysis that determines if we need to keep the code of a method (or
// if it can never run)
class OverriddenVirtualScopesAnalysis {
 private:
  const std::unordered_set<DexType*>& m_scoped_uninstantiable_types;

  std::unordered_map<DexType*, VirtualScopeIdSet>
      m_transitively_defined_virtual_scopes;

  ConcurrentSet<DexType*> m_instantiated_types;
  ConcurrentSet<VirtualScopeId, VirtualScopeIdHasher>
      m_unresolved_super_invoked_virtual_scopes;
  ConcurrentSet<DexMethod*> m_resolved_super_invoked_methods;

  // This helper method initializes
  // m_transitively_defined_virtual_scopes for a particular type
  // finding all virtual scopes which are defined by itself, if actually
  // instantiated, or all instantiable children of the given type.
  void compute_transitively_defined_virtual_scope(
      const std::unordered_map<DexType*, std::unordered_set<DexType*>>&
          instantiable_children,
      const ConcurrentMap<const DexType*, VirtualScopeIdSet>&
          defined_virtual_scopes,
      DexType* t) {
    auto it = m_transitively_defined_virtual_scopes.find(t);
    if (it != m_transitively_defined_virtual_scopes.end()) {
      return;
    }
    auto& res = m_transitively_defined_virtual_scopes[t];
    if (is_instantiated(t)) {
      const auto& own_defined_virtual_scopes =
          defined_virtual_scopes.at_unsafe(t);
      res.insert(own_defined_virtual_scopes.begin(),
                 own_defined_virtual_scopes.end());
      return;
    }
    std::unordered_map<VirtualScopeId, size_t, VirtualScopeIdHasher> counted;
    auto children_it = instantiable_children.find(t);
    if (children_it != instantiable_children.end()) {
      const auto& children = children_it->second;
      for (auto child : children) {
        const auto& defined_virtual_scopes_of_child =
            defined_virtual_scopes.at_unsafe(child);
        for (const auto& virtual_scope : defined_virtual_scopes_of_child) {
          counted[virtual_scope]++;
        }
        compute_transitively_defined_virtual_scope(
            instantiable_children, defined_virtual_scopes, child);
        for (auto& virtual_scope :
             m_transitively_defined_virtual_scopes.at(child)) {
          if (!defined_virtual_scopes_of_child.count(virtual_scope)) {
            counted[virtual_scope]++;
          }
        }
      }
      auto children_size = children.size();
      for (auto& p : counted) {
        if (p.second == children_size) {
          res.insert(p.first);
        }
      }
    }
  }

  // Helper function that finds...
  // 1. all types that are actually instantiated via new-instance, and
  // 2. all targets of an invoke-super, i.e. methods that can be directly
  //    invoked even if overridden by all instantiable children
  void scan_code(const Scope& scope) {
    Timer timer("scan_code");
    walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
      editable_cfg_adapter::iterate(&code, [&](MethodItemEntry& mie) {
        auto insn = mie.insn;
        if (insn->opcode() == OPCODE_NEW_INSTANCE ||
            insn->opcode() == OPCODE_CONST_CLASS) {
          // occurrences of "const-class" doesn't actually mean that the class
          // can be instantiated, but since it's then possible via reflection,
          // we treat it as such
          m_instantiated_types.insert(insn->get_type());
        }
        if (insn->opcode() == OPCODE_INVOKE_SUPER) {
          auto callee_ref = insn->get_method();
          auto callee = resolve_method(callee_ref, MethodSearch::Super, method);
          if (callee == nullptr) {
            m_unresolved_super_invoked_virtual_scopes.insert(
                VirtualScopeId::make(callee_ref));
          } else {
            m_resolved_super_invoked_methods.insert(callee);
          }
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
    });
  }

  bool is_instantiated(DexType* t) const {
    auto cls = type_class(t);
    return root(cls) || !can_rename(cls) || m_instantiated_types.count(t);
  }

 public:
  OverriddenVirtualScopesAnalysis(
      const Scope& scope,
      const std::unordered_set<DexType*>& scoped_uninstantiable_types,
      const std::unordered_map<DexType*, std::unordered_set<DexType*>>&
          instantiable_children)
      : m_scoped_uninstantiable_types(scoped_uninstantiable_types) {
    Timer timer("OverriddenVirtualScopesAnalysis");

    scan_code(scope);

    ConcurrentMap<const DexType*, VirtualScopeIdSet> defined_virtual_scopes;
    walk::parallel::classes(scope, [&](DexClass* cls) {
      VirtualScopeIdSet virtual_scopes;
      for (auto method : cls->get_vmethods()) {
        VirtualScopeId virtual_scope = VirtualScopeId::make(method);
        virtual_scopes.emplace(virtual_scope);
      }
      defined_virtual_scopes.emplace(cls->get_type(),
                                     std::move(virtual_scopes));
    });

    for (auto cls : scope) {
      compute_transitively_defined_virtual_scope(
          instantiable_children, defined_virtual_scopes, cls->get_type());
    }
  }

  bool keep_code(DexMethod* method) const {
    if (is_static(method)) {
      return true;
    }
    if (m_scoped_uninstantiable_types.count(method->get_class())) {
      return false;
    }
    if (!method->is_virtual()) {
      return true;
    }
    if (m_resolved_super_invoked_methods.count(method) ||
        m_unresolved_super_invoked_virtual_scopes.count(
            VirtualScopeId::make(method))) {
      return true;
    }
    if (is_instantiated(method->get_class())) {
      return true;
    }
    VirtualScopeId virtual_scope = VirtualScopeId::make(method);
    return !m_transitively_defined_virtual_scopes.at(method->get_class())
                .count(virtual_scope);
  }
};

} // namespace

// Computes set of uninstantiable types, also looking at the type system to
// find non-external (and non-native)...
// - interfaces that are not annotations, are not root (or unrenameable) and
//   do not contain root (or unrenameable) methods and have no non-abstract
//   classes implementing them, and
// - abstract (non-interface) classes that are not extended by any non-abstract
//   class
std::unordered_set<DexType*>
RemoveUninstantiablesPass::compute_scoped_uninstantiable_types(
    const Scope& scope,
    std::unordered_map<DexType*, std::unordered_set<DexType*>>*
        instantiable_children) {
  // First, we compute types that might possibly be uninstantiable, and classes
  // that we consider instantiable.
  std::unordered_set<DexType*> uninstantiable_types;
  std::unordered_set<const DexClass*> instantiable_classes;
  auto is_interface_instantiable = [](const DexClass* interface) {
    if (is_annotation(interface) || interface->is_external() ||
        root(interface) || !can_rename(interface)) {
      return true;
    }
    for (auto method : interface->get_vmethods()) {
      if (root(method) || !can_rename(method)) {
        return true;
      }
    }
    return false;
  };
  walk::classes(scope, [&](const DexClass* cls) {
    if (type::is_uninstantiable_class(cls->get_type()) ||
        (is_interface(cls) && !is_interface_instantiable(cls))) {
      uninstantiable_types.insert(cls->get_type());
    } else {
      instantiable_classes.insert(cls);
    }
  });
  // Next, we prune the list of possibly uninstantiable types by looking at
  // what instantiable classes implement and extend.
  std::unordered_set<const DexClass*> visited;
  std::function<bool(const DexClass*)> visit;
  visit = [&](const DexClass* cls) {
    if (cls == nullptr || !visited.insert(cls).second) {
      return false;
    }
    if (instantiable_children) {
      (*instantiable_children)[cls->get_super_class()].insert(cls->get_type());
    }
    uninstantiable_types.erase(cls->get_type());
    for (auto interface : *cls->get_interfaces()) {
      visit(type_class(interface));
    }
    return true;
  };
  for (auto cls : instantiable_classes) {
    while (visit(cls)) {
      cls = type_class(cls->get_super_class());
    }
  }
  uninstantiable_types.insert(type::java_lang_Void());
  return uninstantiable_types;
}

void RemoveUninstantiablesPass::run_pass(DexStoresVector& stores,
                                         ConfigFiles&,
                                         PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  std::unordered_map<DexType*, std::unordered_set<DexType*>>
      instantiable_children;
  std::unordered_set<DexType*> scoped_uninstantiable_types =
      compute_scoped_uninstantiable_types(scope, &instantiable_children);
  OverriddenVirtualScopesAnalysis overridden_virtual_scopes_analysis(
      scope, scoped_uninstantiable_types, instantiable_children);
  ConcurrentSet<DexMethod*> uncallable_instance_methods;
  auto stats = walk::parallel::methods<remove_uninstantiables_impl::Stats>(
      scope,
      [&scoped_uninstantiable_types, &overridden_virtual_scopes_analysis,
       &uncallable_instance_methods](DexMethod* method) {
        auto code = method->get_code();
        if (method->rstate.no_optimizations() || code == nullptr) {
          return remove_uninstantiables_impl::Stats();
        }
        always_assert(code->editable_cfg_built());
        if (overridden_virtual_scopes_analysis.keep_code(method)) {
          auto& cfg = code->cfg();
          return remove_uninstantiables_impl::replace_uninstantiable_refs(
              scoped_uninstantiable_types, cfg);
        }
        uncallable_instance_methods.insert(method);
        return remove_uninstantiables_impl::Stats();
      });

  stats += remove_uninstantiables_impl::reduce_uncallable_instance_methods(
      scope, uncallable_instance_methods.move_to_container(),
      [&](const DexMethod* m) { return false; });

  stats.report(mgr);
}

static RemoveUninstantiablesPass s_pass;
