/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdio.h>
#include <unordered_set>

#include "DexClass.h"
#include "DexUtil.h"
#include "PassManager.h"
#include "ReachableClasses.h"
#include "RemoveEmptyClasses.h"
#include "Trace.h"
#include "Walkers.h"

constexpr const char* METRIC_REMOVED_EMPTY_CLASSES =
    "num_empty_classes_removed";

void remove_clinit_if_trivial(DexClass* cls) {
  DexMethod* clinit = cls->get_clinit();
  if (clinit && method::is_trivial_clinit(*clinit->get_code())) {
    cls->remove_method(clinit);
  }
}

bool is_empty_class(DexClass* cls,
                    ConcurrentSet<const DexType*>& class_references) {
  bool empty_class = cls->get_dmethods().empty() &&
                     cls->get_vmethods().empty() &&
                     cls->get_sfields().empty() && cls->get_ifields().empty();
  uint32_t access = cls->get_access();
  auto name = cls->get_type()->get_name()->c_str();
  TRACE(EMPTY, 4, ">> Empty Analysis for %s", name);
  TRACE(EMPTY, 4, "   no methods or fields: %d", empty_class);
  TRACE(EMPTY, 4, "   can delete: %d", can_delete(cls));
  TRACE(EMPTY, 4, "   not interface: %d",
        !(access & DexAccessFlags::ACC_INTERFACE));
  TRACE(EMPTY, 4, "   references: %zu",
        class_references.count(cls->get_type()));
  bool remove = empty_class && can_delete(cls) &&
                !(access & DexAccessFlags::ACC_INTERFACE) &&
                class_references.count(cls->get_type()) == 0;
  TRACE(EMPTY, 4, "   remove: %d", remove);
  return remove;
}

void process_annotation(ConcurrentSet<const DexType*>* class_references,
                        DexAnnotation* annotation) {
  std::vector<DexType*> ltype;
  annotation->gather_types(ltype);
  for (DexType* dextype : ltype) {
    TRACE(EMPTY, 4, "Adding type annotation to keep list: %s",
          dextype->get_name()->c_str());
    class_references->insert(dextype);
  }
}

void process_proto(ConcurrentSet<const DexType*>* class_references,
                   DexMethodRef* meth) {
  // Types referenced in protos.
  auto const& proto = meth->get_proto();
  class_references->insert(type::get_element_type_if_array(proto->get_rtype()));
  for (auto const& ptype : *proto->get_args()) {
    class_references->insert(type::get_element_type_if_array(ptype));
  }
}

void process_code(ConcurrentSet<const DexType*>* class_references,
                  DexMethod* meth,
                  IRCode& code) {
  // Types referenced in code.
  for (auto const& mie : InstructionIterable(meth->get_code())) {
    auto opcode = mie.insn;
    if (opcode->has_type()) {
      auto typ = type::get_element_type_if_array(opcode->get_type());
      TRACE(EMPTY, 4, "Adding type from code to keep list: %s",
            typ->get_name()->c_str());
      class_references->insert(typ);
    } else if (opcode->has_field()) {
      auto const& field = opcode->get_field();
      class_references->insert(
          type::get_element_type_if_array(field->get_class()));
      class_references->insert(
          type::get_element_type_if_array(field->get_type()));
    } else if (opcode->has_method()) {
      auto const& m = opcode->get_method();
      process_proto(class_references, m);
    }
  }
  // Also gather exception types that are caught.
  std::vector<DexType*> catch_types;
  code.gather_catch_types(catch_types);
  for (auto& caught_type : catch_types) {
    class_references->insert(caught_type);
  }
}

size_t remove_empty_classes(Scope& classes) {

  // class_references is a set of type names which represent classes
  // which should not be deleted even if they are deemed to be empty.
  ConcurrentSet<const DexType*> class_references;

  walk::parallel::classes(classes, [&class_references](DexClass* cls) {
    std::vector<DexClass*> singleton_cls{cls};
    walk::annotations(singleton_cls, [&](DexAnnotation* annotation) {
      process_annotation(&class_references, annotation);
    });

    // Check the method protos and all the code.
    walk::methods(singleton_cls, [&class_references](DexMethod* meth) {
      process_proto(&class_references, meth);
      auto code = meth->get_code();
      if (!code) {
        return;
      }
      process_code(&class_references, meth, *code);
    });

    // Ennumerate super classes and remove trivial clinit if the class has any.
    remove_clinit_if_trivial(cls);
    DexType* s = cls->get_super_class();
    class_references.insert(s);

    // Ennumerate fields.
    walk::fields(singleton_cls, [&class_references](DexField* field) {
      class_references.insert(
          type::get_element_type_if_array(field->get_type()));
    });
  });

  size_t classes_before_size = classes.size();
  TRACE(EMPTY, 3, "About to erase classes.");
  classes.erase(remove_if(classes.begin(), classes.end(),
                          [&](DexClass* cls) {
                            return is_empty_class(cls, class_references);
                          }),
                classes.end());

  auto num_classes_removed = classes_before_size - classes.size();
  TRACE(EMPTY, 1, "Empty classes removed: %ld", num_classes_removed);
  return num_classes_removed;
}

void RemoveEmptyClassesPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& /* conf */,
                                      PassManager& mgr) {
  auto scope = build_class_scope(stores);
  auto num_empty_classes_removed = remove_empty_classes(scope);

  mgr.incr_metric(METRIC_REMOVED_EMPTY_CLASSES, num_empty_classes_removed);

  post_dexen_changes(scope, stores);
}

static RemoveEmptyClassesPass s_pass;
