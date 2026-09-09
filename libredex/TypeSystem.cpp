/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeSystem.h"

#include "Debug.h"
#include "DexUtil.h"
#include "RedexContext.h"
#include "TypeUtil.h"

namespace {

void make_instanceof_table(InstanceOfTable& instance_of_table,
                           const ClassHierarchy& hierarchy,
                           const DexType* type,
                           size_t depth = 1) {
  auto& parent_chain = instance_of_table[type];
  auto* const cls = type_class(type);
  if (cls != nullptr) {
    auto* const super = cls->get_super_class();
    if (super != nullptr) {
      const auto& super_chain = instance_of_table.find(super);
      always_assert(super_chain != instance_of_table.end());
      for (const auto& base : super_chain->second) {
        parent_chain.emplace_back(base);
      }
    }
  }
  parent_chain.emplace_back(type);
  always_assert(parent_chain.size() == depth);

  const auto& children = hierarchy.find(type);
  if (children == hierarchy.end()) {
    return;
  }
  for (const auto& child : children->second) {
    make_instanceof_table(instance_of_table, hierarchy, child, depth + 1);
  }
}

void load_interface_children(ClassHierarchy& children, const DexClass* intf) {
  for (const auto& super_intf : *intf->get_interfaces()) {
    children[super_intf].insert(intf->get_type());
    auto* const super_intf_cls = type_class(super_intf);
    if (super_intf_cls != nullptr) {
      load_interface_children(children, super_intf_cls);
    }
  }
}

void load_interface_children(ClassHierarchy& children) {
  g_redex->walk_type_class([&](const DexType* /*type*/, const DexClass* cls) {
    if (!cls->is_external() || !is_interface(cls)) {
      return;
    }
    load_interface_children(children, cls);
  });
}

void load_interface_children(const Scope& scope, ClassHierarchy& children) {
  for (const auto& cls : scope) {
    if (!is_interface(cls)) {
      continue;
    }
    load_interface_children(children, cls);
  }
  load_interface_children(children);
}

} // namespace

namespace {

// The instanceof/interface tables are rooted at java.lang.Object and assume it
// resolves to a DexClass. Building a ClassScopes used to materialize it as a
// side effect (via virt_scope::get_vmethods); now that TypeSystem builds the
// hierarchy directly, it has to say so.
ClassHierarchy build_hierarchy_with_object(const Scope& scope) {
  // No-op when Object already resolves.
  create_object_class();
  return build_type_hierarchy(scope);
}

} // namespace

TypeSystem::TypeSystem(const Scope& scope)
    : m_hierarchy(build_hierarchy_with_object(scope)),
      m_interface_map(build_interface_map(m_hierarchy)) {
  load_interface_children(scope, m_intf_children);
  make_instanceof_interfaces_table();
}

void TypeSystem::get_all_super_interfaces(const DexType* intf,
                                          TypeSet& supers) const {
  auto* const cls = type_class(intf);
  if (cls == nullptr) {
    return;
  }
  for (const auto& super : *cls->get_interfaces()) {
    supers.insert(super);
    get_all_super_interfaces(super, supers);
  }
}

TypeSet TypeSystem::get_all_super_interfaces(const DexType* intf) const {
  TypeSet supers;
  get_all_super_interfaces(intf, supers);
  return supers;
}

TypeSet TypeSystem::get_local_interfaces(const TypeSet& classes) {
  // Collect all implemented interfaces.
  TypeSet implemented_intfs = get_implemented_interfaces(classes);

  // Remove interfaces that are implemented by other classes too.
  for (auto it = implemented_intfs.begin(); it != implemented_intfs.end();) {
    bool keep = true;

    const auto& implementors = get_implementors(*it);
    for (const auto& cls : implementors) {
      if (classes.count(cls) == 0u) {
        keep = false;
        break;
      }
    }

    if (keep) {
      ++it;
    } else {
      it = implemented_intfs.erase(it);
    }
  }

  return implemented_intfs;
}

void TypeSystem::make_instanceof_interfaces_table() {
  TypeVector no_parents;
  for (const auto& children_it : UnorderedIterable(m_hierarchy)) {
    const auto* const parent = children_it.first;
    auto* const parent_cls = type_class(parent);
    if (parent_cls != nullptr) {
      continue;
    }
    no_parents.emplace_back(parent);
  }
  no_parents.emplace_back(type::java_lang_Object());
  for (const auto& root : no_parents) {
    make_instanceof_table(m_instanceof_table, m_hierarchy, root);
  }
  for (const auto& root : no_parents) {
    make_interfaces_table(root);
  }
}

void TypeSystem::make_interfaces_table(const DexType* type) {
  auto* const cls = type_class(type);
  if (cls != nullptr) {
    auto* const super = cls->get_super_class();
    if (super != nullptr) {
      const auto& parent_intfs = m_interfaces.find(super);
      if (parent_intfs != m_interfaces.end()) {
        m_interfaces[type].insert(parent_intfs->second.begin(),
                                  parent_intfs->second.end());
      }
    }
    for (const auto& intf : *cls->get_interfaces()) {
      m_interfaces[type].insert(intf);
      get_all_super_interfaces(intf, m_interfaces[type]);
    }
  }

  const auto& children = m_hierarchy.find(type);
  if (children == m_hierarchy.end()) {
    return;
  }
  for (const auto& child : children->second) {
    make_interfaces_table(child);
  }
}
