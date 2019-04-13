/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdio.h>
#include <unordered_set>

#include "Walkers.h"
#include "ReachableClasses.h"
#include "RemoveEmptyClasses.h"
#include "DexClass.h"
#include "DexUtil.h"

constexpr const char* METRIC_REMOVED_EMPTY_CLASSES =
  "num_empty_classes_removed";

bool is_empty_class(DexClass* cls,
                    std::unordered_set<const DexType*>& class_references) {
  bool empty_class = cls->get_dmethods().empty() &&
  cls->get_vmethods().empty() &&
  cls->get_sfields().empty() &&
  cls->get_ifields().empty();
  uint32_t access = cls->get_access();
  auto name = cls->get_type()->get_name()->c_str();
  TRACE(EMPTY, 4, ">> Empty Analysis for %s\n", name);
  TRACE(EMPTY, 4, "   no methods or fields: %d\n", empty_class);
  TRACE(EMPTY, 4, "   can delete: %d\n", can_delete(cls));
  TRACE(EMPTY, 4, "   not interface: %d\n",
      !(access & DexAccessFlags::ACC_INTERFACE));
  TRACE(EMPTY, 4, "   references: %d\n",
      class_references.count(cls->get_type()));
  bool remove =
         empty_class &&
         can_delete(cls) &&
         !(access & DexAccessFlags::ACC_INTERFACE) &&
         class_references.count(cls->get_type()) == 0;
  TRACE(EMPTY, 4, "   remove: %d\n", remove);
  return remove;
}

void process_annotation(
    std::unordered_set<const DexType*>* class_references,
    DexAnnotation* annotation) {
  std::vector<DexType*> ltype;
  annotation->gather_types(ltype);
  for (DexType* dextype : ltype) {
    TRACE(EMPTY, 4, "Adding type annotation to keep list: %s\n",
          dextype->get_name()->c_str());
    class_references->insert(dextype);
  }
}

DexType* array_base_type(DexType* type) {
  while (is_array(type)) {
    type = get_array_type(type);
  }
  return type;
}

void process_proto(std::unordered_set<const DexType*>* class_references,
                   DexMethodRef* meth) {
  // Types referenced in protos.
  auto const& proto = meth->get_proto();
  class_references->insert(array_base_type(proto->get_rtype()));
  for (auto const& ptype : proto->get_args()->get_type_list()) {
    class_references->insert(array_base_type(ptype));
  }
}

void process_code(std::unordered_set<const DexType*>* class_references,
                  DexMethod* meth,
                  IRCode& code) {
  process_proto(class_references, meth);
  // Types referenced in code.
  for (auto const& mie : InstructionIterable(meth->get_code())) {
    auto opcode = mie.insn;
    if (opcode->has_type()) {
      auto typ = array_base_type(opcode->get_type());
      TRACE(EMPTY, 4, "Adding type from code to keep list: %s\n",
            typ->get_name()->c_str());
      class_references->insert(typ);
    }
    if (opcode->has_field()) {
      auto const& field = opcode->get_field();
      class_references->insert(array_base_type(field->get_class()));
      class_references->insert(array_base_type(field->get_type()));
    }
    if (opcode->has_method()) {
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
  std::unordered_set<const DexType*> class_references;

  walk::annotations(classes, [&](DexAnnotation* annotation)
    { process_annotation(&class_references, annotation); });

  walk::code(classes,
            [](DexMethod*) { return true; },
            [&](DexMethod* meth, IRCode& code)
               { process_code(&class_references, meth, code); });

  size_t classes_before_size = classes.size();

  // Ennumerate super classes.
  for (auto& cls : classes) {
    DexType* s = cls->get_super_class();
    class_references.insert(s);
  }

  TRACE(EMPTY, 3, "About to erase classes.\n");
  classes.erase(remove_if(classes.begin(), classes.end(),
    [&](DexClass* cls) { return is_empty_class(cls, class_references); }),
    classes.end());

  auto num_classes_removed = classes_before_size - classes.size();
  TRACE(EMPTY, 1, "Empty classes removed: %ld\n", num_classes_removed);
  return num_classes_removed;
}

void RemoveEmptyClassesPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles& /* conf */,
                                      PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(EMPTY, 1, "RemoveEmptyClassesPass not run because no ProGuard configuration was provided.");
    return;
  }
  auto scope = build_class_scope(stores);
  auto num_empty_classes_removed = remove_empty_classes(scope);

  mgr.incr_metric(METRIC_REMOVED_EMPTY_CLASSES, num_empty_classes_removed);

  post_dexen_changes(scope, stores);
}

static RemoveEmptyClassesPass s_pass;
