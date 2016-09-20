/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <stdio.h>
#include <unordered_set>

#include "Walkers.h"
#include "ReachableClasses.h"
#include "RemoveEmptyClasses.h"
#include "DexClass.h"
#include "DexUtil.h"

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

void process_annotation(std::unordered_set<const DexType*>* class_references,
  DexAnnotation* annotation) {
    if (annotation->runtime_visible()) {
      auto elements = annotation->anno_elems();
      for (const DexAnnotationElement& element : elements) {
        DexEncodedValue* evalue = element.encoded_value;
        std::vector<DexType*> ltype;
        evalue->gather_types(ltype);
        for (DexType* dextype : ltype) {
          TRACE(EMPTY, 4, "Adding type annotation to keep list: %s\n",
                dextype->get_name()->c_str());
          class_references->insert(dextype);
        }
      }
    }
}

DexType* array_base_type(DexType* type) {
  while (is_array(type)) {
    type = get_array_type(type);
  }
  return type;
}

void process_proto(std::unordered_set<const DexType*>* class_references,
                   DexMethod* meth) {
  // Types referenced in protos.
  auto const& proto = meth->get_proto();
  class_references->insert(array_base_type(proto->get_rtype()));
  for (auto const& ptype : proto->get_args()->get_type_list()) {
    class_references->insert(array_base_type(ptype));
  }
}  

void process_code(std::unordered_set<const DexType*>* class_references,
                  DexMethod* meth,
                  DexCode& code) {
  process_proto(class_references, meth);
  // Types referenced in code.
  auto opcodes = code.get_instructions();
  for (const auto& opcode : opcodes) {
    if (opcode->has_types()) {
      auto typeop = static_cast<DexOpcodeType*>(opcode);
      auto typ = array_base_type(typeop->get_type());
      TRACE(EMPTY, 4, "Adding type from code to keep list: %s\n",
            typ->get_name()->c_str());
      class_references->insert(typ);
    }
    if (opcode->has_fields()) {
      auto const& field = static_cast<DexOpcodeField*>(opcode)->field();
      class_references->insert(array_base_type(field->get_class()));
      class_references->insert(array_base_type(field->get_type()));
    }
    if (opcode->has_methods()) {
      auto const& m = static_cast<DexOpcodeMethod*>(opcode)->get_method();
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

void remove_empty_classes(Scope& classes) {

  // class_references is a set of type names which represent classes
  // which should not be deleted even if they are deemed to be empty.
  std::unordered_set<const DexType*> class_references;

  walk_annotations(classes, [&](DexAnnotation* annotation)
    { process_annotation(&class_references, annotation); });

  walk_code(classes,
            [](DexMethod*) { return true; },
            [&](DexMethod* meth, DexCode& code)
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

  TRACE(EMPTY, 1, "Empty classes removed: %ld\n",
    classes_before_size - classes.size());
}

void RemoveEmptyClassesPass::run_pass(
    DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  auto scope = build_class_scope(stores);
  remove_empty_classes(scope);
  post_dexen_changes(scope, stores);
}

static RemoveEmptyClassesPass s_pass;
