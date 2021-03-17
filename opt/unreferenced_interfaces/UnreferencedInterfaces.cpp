/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
  const auto super_intfs = intf->get_interfaces()->get_type_list();
  for (const auto super : super_intfs) {
    interfaces.insert(super);
    auto super_intf = type_class(super);
    if (super_intf != nullptr) {
      get_super_interfaces(interfaces, super_intf);
    }
  }
}

void get_interfaces(TypeSet& interfaces, DexClass* cls) {
  const auto intfs = cls->get_interfaces()->get_type_list();
  for (const auto& intf : intfs) {
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

  std::vector<const DexType*> external;
  for (const auto& intf : candidates) {
    const auto cls = type_class(intf);
    if (cls == nullptr || cls->is_external()) {
      external.emplace_back(intf);
    }
  }
  for (const auto& intf : external) {
    metric.external += candidates.erase(intf);
  }

  return candidates;
}

void remove_referenced(const Scope& scope,
                       TypeSet& candidates,
                       UnreferencedInterfacesPass::Metric& metric) {

  const auto check_type = [&](DexType* t, size_t& count) {
    const auto type = type::get_element_type_if_array(t);
    if (candidates.count(type) > 0) {
      candidates.erase(type);
      count++;
    }
  };

  walk::fields(scope, [&](DexField* field) {
    check_type(field->get_type(), metric.field_refs);
  });
  walk::methods(scope, [&](DexMethod* meth) {
    const auto proto = meth->get_proto();
    check_type(proto->get_rtype(), metric.sig_refs);
    for (const auto& type : proto->get_args()->get_type_list()) {
      check_type(type, metric.sig_refs);
    }
  });
  walk::annotations(scope, [&](DexAnnotation* anno) {
    std::vector<DexType*> types_in_anno;
    anno->gather_types(types_in_anno);
    for (const auto& type : types_in_anno) {
      check_type(type, metric.anno_refs);
    }
  });
  walk::opcodes(
      scope,
      [](DexMethod*) { return true; },
      [&](DexMethod*, IRInstruction* insn) {
        if (insn->has_type()) {
          check_type(insn->get_type(), metric.insn_refs);
          return;
        }

        std::vector<DexType*> types_in_insn;
        if (insn->has_field()) {
          insn->get_field()->gather_types_shallow(types_in_insn);
        } else if (insn->has_method()) {
          insn->get_method()->gather_types_shallow(types_in_insn);
        }
        for (const auto type : types_in_insn) {
          check_type(type, metric.insn_refs);
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
          check_type(meth->get_class(), metric.insn_refs);
          return;
        }

        // the method resolved to nothing which is odd but there are
        // cases where it happens (OS versions, virtual call on an
        // unimplemented interface method, etc.).
        // To be safe let's remove every interface involved in this branch
        const auto& cls = type_class(insn->get_method()->get_class());
        if (cls == nullptr) return;

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
      });
}

bool implements_removables(const TypeSet& removable, DexClass* cls) {
  for (const auto intf : cls->get_interfaces()->get_type_list()) {
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
  for (const auto& super_intf : cls_intf->get_interfaces()->get_type_list()) {
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
  for (const auto& intf : cls->get_interfaces()->get_type_list()) {
    if (removable.count(intf) == 0) {
      new_intfs.insert(intf);
      continue;
    }
    get_impls(intf, removable, new_intfs);
  }
  std::deque<DexType*> deque;
  for (const auto& intf : new_intfs) {
    deque.emplace_back(intf);
  }
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

  TRACE(UNREF_INTF, 1, "candidates %ld", m_metric.candidates);
  TRACE(UNREF_INTF, 1, "external %ld", m_metric.external);
  TRACE(UNREF_INTF, 1, "on abstract classes %ld", m_metric.on_abstract_cls);
  TRACE(UNREF_INTF, 1, "field references %ld", m_metric.field_refs);
  TRACE(UNREF_INTF, 1, "signature references %ld", m_metric.sig_refs);
  TRACE(UNREF_INTF, 1, "instruction references %ld", m_metric.insn_refs);
  TRACE(UNREF_INTF, 1, "annotation references %ld", m_metric.anno_refs);
  TRACE(UNREF_INTF, 1, "unresolved methods %ld", m_metric.unresolved_meths);
  TRACE(UNREF_INTF, 1, "updated implementations %ld", m_metric.updated_impls);
  TRACE(UNREF_INTF, 1, "removable %ld", m_metric.removed);

  mgr.set_metric("updated implementations", m_metric.updated_impls);
  mgr.set_metric("removed_interfaces", m_metric.removed);
}

static UnreferencedInterfacesPass s_pass;
