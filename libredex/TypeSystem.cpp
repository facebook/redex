/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeSystem.h"

#include "DexUtil.h"
#include "Resolver.h"
#include "Show.h"
#include "Timer.h"
#include "Trace.h"

namespace {

void make_instanceof_table(InstanceOfTable& instance_of_table,
                           const ClassHierarchy& hierarchy,
                           const DexType* type,
                           size_t depth = 1) {
  auto& parent_chain = instance_of_table[type];
  const auto cls = type_class(type);
  if (cls != nullptr) {
    const auto super = cls->get_super_class();
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
  if (children == hierarchy.end()) return;
  for (const auto& child : children->second) {
    make_instanceof_table(instance_of_table, hierarchy, child, depth + 1);
  }
}

void load_interface_children(ClassHierarchy& children, const DexClass* intf) {
  for (const auto& super_intf : *intf->get_interfaces()) {
    children[super_intf].insert(intf->get_type());
    const auto super_intf_cls = type_class(super_intf);
    if (super_intf_cls != nullptr) {
      load_interface_children(children, super_intf_cls);
    }
  }
}

void load_interface_children(ClassHierarchy& children) {
  g_redex->walk_type_class([&](const DexType* type, const DexClass* cls) {
    if (!cls->is_external() || !is_interface(cls)) return;
    load_interface_children(children, cls);
  });
}

void load_interface_children(const Scope& scope, ClassHierarchy& children) {
  for (const auto& cls : scope) {
    if (!is_interface(cls)) continue;
    load_interface_children(children, cls);
  }
  load_interface_children(children);
}

} // namespace

const TypeSet TypeSystem::empty_set = TypeSet();
const TypeVector TypeSystem::empty_vec = TypeVector();

TypeSystem::TypeSystem(const Scope& scope) : m_class_scopes(scope) {
  load_interface_children(scope, m_intf_children);
  make_instanceof_interfaces_table();
}

void TypeSystem::get_all_super_interfaces(const DexType* intf,
                                          TypeSet& supers) const {
  const auto cls = type_class(intf);
  if (cls == nullptr) return;
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
      if (!classes.count(cls)) {
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

const VirtualScope* TypeSystem::find_virtual_scope(
    const DexMethod* meth) const {

  const auto match = [](const DexMethod* meth1, const DexMethod* meth2) {
    return meth1->get_name() == meth2->get_name() &&
           meth1->get_proto() == meth2->get_proto();
  };

  auto type = meth->get_class();
  while (type != nullptr) {
    TRACE(VIRT, 5, "check... %s", SHOW(type));
    for (const auto& scope : m_class_scopes.get(type)) {
      TRACE(VIRT, 5, "check... %s", SHOW(scope->methods[0].first));
      if (match(scope->methods[0].first, meth)) {
        TRACE(VIRT, 5, "return scope");
        return scope;
      }
    }
    const auto cls = type_class(type);
    if (cls == nullptr) break;
    type = cls->get_super_class();
  }

  return nullptr;
}

std::vector<const DexMethod*> TypeSystem::select_from(
    const VirtualScope* scope, const DexType* type) const {
  std::vector<const DexMethod*> refined_scope;
  std::unordered_map<const DexType*, DexMethod*> non_child_methods;
  bool found_root_method = false;
  for (const auto& method : scope->methods) {
    if (is_subtype(type, method.first->get_class())) {
      found_root_method =
          found_root_method || type == method.first->get_class();
      refined_scope.emplace_back(method.first);
    } else {
      non_child_methods[method.first->get_class()] = method.first;
    }
  }
  if (!found_root_method) {
    const auto& parents = parent_chain(type);
    for (auto parent = parents.rbegin(); parent != parents.rend(); ++parent) {
      const auto& meth = non_child_methods.find(*parent);
      if (meth == non_child_methods.end()) continue;
      refined_scope.emplace_back(meth->second);
      break;
    }
  }
  return refined_scope;
}

void TypeSystem::make_instanceof_interfaces_table() {
  TypeVector no_parents;
  const auto& hierarchy = m_class_scopes.get_class_hierarchy();
  for (const auto& children_it : hierarchy) {
    const auto parent = children_it.first;
    const auto parent_cls = type_class(parent);
    if (parent_cls != nullptr) continue;
    no_parents.emplace_back(parent);
  }
  no_parents.emplace_back(type::java_lang_Object());
  for (const auto& root : no_parents) {
    make_instanceof_table(m_instanceof_table, hierarchy, root);
  }
  for (const auto& root : no_parents) {
    make_interfaces_table(root);
  }
}

void TypeSystem::make_interfaces_table(const DexType* type) {
  const auto cls = type_class(type);
  if (cls != nullptr) {
    const auto super = cls->get_super_class();
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

  const auto& hierarchy = m_class_scopes.get_class_hierarchy();
  const auto& children = hierarchy.find(type);
  if (children == hierarchy.end()) return;
  for (const auto& child : children->second) {
    make_interfaces_table(child);
  }
}

void TypeSystem::select_methods(const VirtualScope& scope,
                                const std::unordered_set<DexType*>& types,
                                std::unordered_set<DexMethod*>& methods) const {
  TRACE(VIRT, 1, "select_methods make filter");
  std::unordered_set<DexType*> filter;
  filter.insert(types.begin(), types.end());

  TRACE(VIRT, 1, "select_methods make type_method map");
  std::unordered_map<const DexType*, DexMethod*> type_method;
  for (const auto& vmeth : scope.methods) {
    const auto meth = vmeth.first;
    if (!meth->is_def()) continue;
    type_method[meth->get_class()] = meth;
  }

  TRACE(VIRT, 1, "select_methods walk hierarchy");
  while (!filter.empty()) {
    const auto type = *filter.begin();
    filter.erase(filter.begin());
    TRACE(VIRT, 1, "check... %s", SHOW(type));
    if (!is_subtype(scope.type, type)) continue;
    const auto& meth = type_method.find(type);
    if (meth != type_method.end()) {
      methods.insert(meth->second);
      continue;
    }
    const auto super = type_class(type)->get_super_class();
    if (super == nullptr) continue;
    if (types.count(super) > 0) continue;
    filter.insert(super);
  }
}

void TypeSystem::select_methods(const InterfaceScope& scope,
                                const std::unordered_set<DexType*>& types,
                                std::unordered_set<DexMethod*>& methods) const {
  for (const auto& virt_scope : scope) {
    select_methods(*virt_scope, types, methods);
  }
}
