/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UnreferencedInterfaces.h"

#include "ClassHierarchy.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IROpcode.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {

void update_scope(const TypeSet& removable, Scope& scope) {
  if (removable.empty()) return;
  Scope tscope(scope);
  scope.clear();
  for (DexClass* cls : tscope) {
    if (removable.count(cls->get_type()) > 0) {
      TRACE(UNREF_INTF, 3, "Removing interface %s", SHOW(cls));
    } else {
      scope.push_back(cls);
    }
  }
}

void get_super_interfaces(TypeSet& interfaces, DexClass* intf) {
  for (const auto super : *intf->get_interfaces()) {
    interfaces.insert(super);
    auto super_intf = type_class(super);
    if (super_intf != nullptr) {
      get_super_interfaces(interfaces, super_intf);
    }
  }
}

void get_interfaces(TypeSet& interfaces, DexClass* cls) {
  for (const auto& intf : *cls->get_interfaces()) {
    interfaces.insert(intf);
    const auto intf_cls = type_class(intf);
    if (intf_cls != nullptr) {
      get_super_interfaces(interfaces, intf_cls);
    }
  }
  const auto super = type_class(cls->get_super_class());
  if (super != nullptr) {
    get_interfaces(interfaces, super);
  }
}

// Collect candidate interfaces that could be safe to remove
TypeSet collect_interfaces(const Scope& scope,
                           UnreferencedInterfacesPass::Metric& metric) {
  TypeSet candidates;
  for (const auto& cls : scope) {
    if (!is_interface(cls)) continue;
    if (!can_delete(cls)) continue;
    if (!cls->get_sfields().empty()) continue;
    always_assert(!cls->is_external());
    candidates.insert(cls->get_type());
    metric.candidates++;
  }

  // Exclude interfaces implemented by abstract classes.
  // Things could get complicated.
  for (const auto& cls : scope) {
    if (is_interface(cls) || !is_abstract(cls)) continue;
    // Only abstract classes
    TypeSet implemented;
    get_interfaces(implemented, cls);
    for (const auto& intf : implemented) {
      if (candidates.count(intf) > 0) {
        candidates.erase(intf);
        metric.on_abstract_cls++;
      }
    }
  }

  return candidates;
}

void remove_referenced(const Scope& scope,
                       TypeSet& candidates,
                       UnreferencedInterfacesPass::Metric& metric) {

  ConcurrentSet<const DexType*> concurrent_candidates_to_erase;
  const auto check_type = [&](DexType* t) {
    const auto type = type::get_element_type_if_array(t);
    if (candidates.count(type) > 0) {
      concurrent_candidates_to_erase.insert(type);
    }
  };
  const auto process_candidates_to_erase = [&](size_t& count) {
    for (auto type : concurrent_candidates_to_erase) {
      if (candidates.count(type) > 0) {
        candidates.erase(type);
        count++;
      }
    }
    concurrent_candidates_to_erase.clear();
  };

  walk::parallel::fields(
      scope, [&](DexField* field) { check_type(field->get_type()); });
  process_candidates_to_erase(metric.field_refs);

  walk::parallel::methods(scope, [&](DexMethod* meth) {
    const auto proto = meth->get_proto();
    check_type(proto->get_rtype());
    for (const auto& type : *proto->get_args()) {
      check_type(type);
    }
  });
  process_candidates_to_erase(metric.sig_refs);

  walk::parallel::annotations(scope, [&](DexAnnotation* anno) {
    std::vector<DexType*> types_in_anno;
    anno->gather_types(types_in_anno);
    for (const auto& type : types_in_anno) {
      check_type(type);
    }
  });
  process_candidates_to_erase(metric.anno_refs);

  ConcurrentSet<DexClass*> concurrent_unresolved_classes;
  walk::parallel::opcodes(
      scope,
      [](DexMethod*) { return true; },
      [&](DexMethod*, IRInstruction* insn) {
        if (insn->has_type()) {
          check_type(insn->get_type());
          return;
        }

        std::vector<DexType*> types_in_insn;
        if (insn->has_field()) {
          insn->get_field()->gather_types_shallow(types_in_insn);
        } else if (insn->has_method()) {
          insn->get_method()->gather_types_shallow(types_in_insn);
        }
        for (const auto type : types_in_insn) {
          check_type(type);
        }

        if (!insn->has_method()) return;
        const auto opcode = insn->opcode();
        DexMethod* meth = nullptr;
        if (opcode == OPCODE_INVOKE_VIRTUAL) {
          meth = resolve_method(insn->get_method(), MethodSearch::Virtual);
        } else if (opcode == OPCODE_INVOKE_INTERFACE) {
          meth = resolve_method(insn->get_method(), MethodSearch::Interface);
        } else {
          return;
        }
        if (meth != nullptr) {
          check_type(meth->get_class());
          return;
        }

        // the method resolved to nothing which is odd but there are
        // cases where it happens (OS versions, virtual call on an
        // unimplemented interface method, etc.).
        // To be safe let's remove every interface involved in this branch
        const auto& cls = type_class(insn->get_method()->get_class());
        if (cls == nullptr) return;
        concurrent_unresolved_classes.insert(cls);
      });
  process_candidates_to_erase(metric.insn_refs);

  for (auto cls : concurrent_unresolved_classes) {
    TypeSet intfs;
    if (is_interface(cls)) {
      intfs.insert(cls->get_type());
      get_super_interfaces(intfs, cls);
    } else {
      get_interfaces(intfs, cls);
    }
    for (const auto& intf : intfs) {
      metric.unresolved_meths += candidates.erase(intf);
    }
  }
}

bool implements_removables(const TypeSet& removable, DexClass* cls) {
  for (const auto intf : *cls->get_interfaces()) {
    if (removable.count(intf) > 0) {
      return true;
    }
  }
  return false;
}

void get_impls(DexType* intf,
               const TypeSet& removable,
               std::set<DexType*, dextypes_comparator>& new_intfs) {
  const auto cls_intf = type_class(intf);
  if (cls_intf == nullptr) return;
  for (const auto& super_intf : *cls_intf->get_interfaces()) {
    if (removable.count(super_intf) == 0) {
      new_intfs.insert(super_intf);
      continue;
    }
    get_impls(super_intf, removable, new_intfs);
  }
};

void set_new_impl_list(const TypeSet& removable, DexClass* cls) {
  TRACE(UNREF_INTF, 3, "Changing implements for %s:\n\tfrom %s", SHOW(cls),
        SHOW(cls->get_interfaces()));
  std::set<DexType*, dextypes_comparator> new_intfs;
  for (const auto& intf : *cls->get_interfaces()) {
    if (removable.count(intf) == 0) {
      new_intfs.insert(intf);
      continue;
    }
    get_impls(intf, removable, new_intfs);
  }
  DexTypeList::ContainerType deque{new_intfs.begin(), new_intfs.end()};
  auto implements = DexTypeList::make_type_list(std::move(deque));
  TRACE(UNREF_INTF, 3, "\tto %s", SHOW(implements));
  cls->set_interfaces(implements);
}

} // namespace

void UnreferencedInterfacesPass::run_pass(DexStoresVector& stores,
                                          ConfigFiles& /*cfg*/,
                                          PassManager& mgr) {
  auto scope = build_class_scope(stores);

  auto removable = collect_interfaces(scope, m_metric);
  remove_referenced(scope, removable, m_metric);

  for (const auto& cls : scope) {
    if (!implements_removables(removable, cls)) {
      continue;
    }
    m_metric.updated_impls++;
    set_new_impl_list(removable, cls);
  }
  m_metric.removed = removable.size();

  update_scope(removable, scope);
  post_dexen_changes(scope, stores);

  TRACE(UNREF_INTF, 1, "candidates %zu", m_metric.candidates);
  TRACE(UNREF_INTF, 1, "on abstract classes %zu", m_metric.on_abstract_cls);
  TRACE(UNREF_INTF, 1, "field references %zu", m_metric.field_refs);
  TRACE(UNREF_INTF, 1, "signature references %zu", m_metric.sig_refs);
  TRACE(UNREF_INTF, 1, "instruction references %zu", m_metric.insn_refs);
  TRACE(UNREF_INTF, 1, "annotation references %zu", m_metric.anno_refs);
  TRACE(UNREF_INTF, 1, "unresolved methods %zu", m_metric.unresolved_meths);
  TRACE(UNREF_INTF, 1, "updated implementations %zu", m_metric.updated_impls);
  TRACE(UNREF_INTF, 1, "removable %zu", m_metric.removed);

  mgr.set_metric("on abstract classes", m_metric.on_abstract_cls);
  mgr.set_metric("updated implementations", m_metric.updated_impls);
  mgr.set_metric("removed_interfaces", m_metric.removed);
}

static UnreferencedInterfacesPass s_pass;
