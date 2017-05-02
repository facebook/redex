/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "TargetTypeHierarchy.h"

namespace {

void collect_interfaces_helper(const DexType* type, TypeSet& cls_intfs) {
  cls_intfs.insert(type);
  const auto& cls = type_class(type);
  always_assert(cls != nullptr);
  const auto& intfs = cls->get_interfaces()->get_type_list();
  for (const auto& intf : intfs) {
    collect_interfaces_helper(intf, cls_intfs);
  }
}

void collect_interfaces(TypeSet& model_classes, TypeSet& interfaces) {
  for (const auto& type : model_classes) {
    const auto& cls = type_class(type);
    const auto& intfs = cls->get_interfaces()->get_type_list();
    for (const auto& intf : intfs) {
      collect_interfaces_helper(intf, interfaces);
    }
  }
}

}

TargetTypeHierarchy TargetTypeHierarchy::build_gql_type_hierarchy(
    const Scope& scope, const ClassHierarchy& hierarchy) {
  TargetTypeHierarchy gql_model;
  gql_model.name = "GQL";
  const auto& tree = DexType::make_type("Lcom/facebook/graphservice/Tree;");
  get_all_children(hierarchy, tree, gql_model.model_classes);
  const auto& base =
      DexType::make_type("Lcom/facebook/graphql/modelutil/BaseModel;");
  get_all_children(hierarchy, base, gql_model.model_classes);
  collect_interfaces(gql_model.model_classes, gql_model.interfaces);
  gql_model.print();
  return gql_model;
}

TargetTypeHierarchy TargetTypeHierarchy::build_cs_type_hierarchy(
    const Scope& scope, const ClassHierarchy& hierarchy) {
  const auto& jsr = DexType::make_type("Lcom/facebook/java2js/JSReadable;");
  const auto& jsref = DexType::make_type(
      "Lcomfacebook/flowtype/components/HasLocalJSRefInitializer");
  TargetTypeHierarchy cs_model;
  cs_model.name = "CS";
  for (const auto& cls : scope) {
    const auto& intfs = cls->get_interfaces()->get_type_list();
    if (intfs.empty()) continue;
    int match = 0;
    for (const auto& intf : intfs) {
      if (intf == jsr) {
        match++;
      } else if (intf == jsref) {
        match++;
      }
    }
    if (match == 2) {
      cs_model.model_classes.insert(cls->get_type());
    }
  }

  TypeSet children;
  for (const auto& type : cs_model.model_classes) {
    get_all_children(hierarchy, type, children);
  }
  cs_model.model_classes.insert(children.begin(), children.end());
  collect_interfaces(cs_model.model_classes, cs_model.interfaces);
  cs_model.print();
  return cs_model;
}

void TargetTypeHierarchy::print() const {
  TRACE(TERA, 1,
    "**** %s Model [%ld]\n"
    "+ Implemented Interfaces [%ld]\n"
    "+ Classes in Namespaces [%ld]\n",
    name,
    model_classes.size(),
    interfaces.size()
  );
}
