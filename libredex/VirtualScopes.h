/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "ClassHierarchy.h" // TypeSet
#include "DeterministicContainers.h"
#include "DexClass.h"

namespace method_override_graph {
class Graph;
} // namespace method_override_graph

/*
 * A modern, encapsulated "virtual scope" API for ClassMerging (and future
 * consumers), backed by `MethodOverrideGraph` (MOG) + the class hierarchy
 * instead of the legacy signature-map machinery in `VirtualScope.h`.
 *
 * It reproduces the observable result of legacy `virt_scope::ClassScopes::get`
 * exactly -- verified per shape by test/unit/VirtualScopesDifferentialTest --
 * so the ClassMerging migration on top is behavior-preserving (NFC). The point
 * of being MOG-backed (rather than delegating to legacy) is that it lets the
 * legacy `VirtualScope`/`ClassScopes` be retired once all consumers move over.
 *
 * Terminology (unchanged):
 *  - virtual scope: the virtual methods sharing one signature connected through
 *    class-hierarchy overriding, anchored at a `root` type.
 *  - the scope's methods: the DexMethods in that chain, in legacy order (root's
 *    top def first, then a pre-order DFS of overrides, siblings by type name).
 *    `methods()[0]` may be a synthetic non-def "miranda" placeholder when the
 *    scope is an interface obligation the root declares but does not define.
 *  - top def: `methods()[0]`.
 */
namespace virtual_scope {

class VirtualScope {
 public:
  // The type this scope is anchored at.
  const DexType* root() const { return m_root; }

  // The methods in the scope, in legacy order. `methods()[0]` is `top_def()`
  // and may be a synthetic miranda placeholder.
  const std::vector<const DexMethod*>& methods() const { return m_methods; }

  // The method anchoring the scope (`methods()[0]`); may be a non-def miranda.
  const DexMethod* top_def() const { return m_top_def; }

  // Interfaces whose method this scope implements (empty for a plain class
  // scope that honors no interface).
  const UnorderedSet<const DexType*>& implemented_interfaces() const {
    return m_interfaces;
  }

  // Does this scope contribute to interface resolution? (legacy
  // `is_impl_scope`)
  bool implements_interface() const { return !m_interfaces.empty(); }

  // Does the scope contain at least one real method definition? Matches legacy
  // `VirtualScope::has_def` -- any member `is_def()`, abstract methods
  // included.
  bool has_def() const { return m_has_def; }

  // A single non-interface method introducing its signature with no overrides
  // -- devirtualizable. Exactly legacy `is_non_virtual_scope`: the scope's top
  // method's flags are precisely `TOP_DEF|FINAL` (computed during the build).
  bool is_effectively_final() const { return m_non_virtual; }

  // True if any member method may escape -- its class (or an ancestor /
  // descendant) implements an unresolvable interface, so the whole branch is
  // poisoned. Mirrors legacy per-method `VirtualFlags::ESCAPED`; obfuscation's
  // renamer treats an escaped scope as not renamable.
  bool escaped() const { return m_escaped; }

 private:
  friend class VirtualScopes;
  VirtualScope() = default;

  const DexType* m_root{nullptr};
  const DexMethod* m_top_def{nullptr};
  std::vector<const DexMethod*> m_methods;
  UnorderedSet<const DexType*> m_interfaces;
  bool m_has_def{false};
  bool m_non_virtual{false};
  bool m_escaped{false};
};

class VirtualScopes {
 public:
  explicit VirtualScopes(const Scope& scope);
  ~VirtualScopes();

  VirtualScopes(const VirtualScopes&) = delete;
  VirtualScopes& operator=(const VirtualScopes&) = delete;

  // The virtual scopes anchored at `type` (empty if none). Pointers are stable
  // for the lifetime of this object.
  const std::vector<const VirtualScope*>& at(const DexType* type) const;

  // The virtual scope `meth` belongs to -- the scope rooted at the highest
  // ancestor along `meth`'s superclass chain that introduces its signature --
  // or nullptr if `meth` belongs to no scope. Reproduces legacy
  // `TypeSystem::find_virtual_scope`.
  const VirtualScope* find(const DexMethod* meth) const;

  // For each (method name, proto) that has at least one interface-implementing
  // scope, invoke `walker(name, proto, scopes, interfaces)` with all such
  // scopes and the union of the interfaces they implement. Groups are visited
  // in (name, proto) order (dexstrings/dexprotos comparators). Reproduces
  // legacy `ClassScopes::walk_all_intf_scopes`.
  void walk_interface_scopes(
      const std::function<void(const DexString*,
                               const DexProto*,
                               const std::vector<const VirtualScope*>&,
                               const TypeSet&)>& walker) const;

 private:
  std::vector<std::unique_ptr<VirtualScope>> m_storage;
  UnorderedMap<const DexType*, std::vector<const VirtualScope*>> m_by_root;
};

} // namespace virtual_scope
