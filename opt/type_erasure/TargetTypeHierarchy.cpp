/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "TargetTypeHierarchy.h"

void collect_classes(
  const ClassHierarchy& ch,
  const DexType* base,
  TypeSet& target_classes
) {
  TypeSet children = get_children(ch, base);
  for (const auto& child : children) {
    collect_classes(ch, child, target_classes);
    target_classes.insert(child);
  }
}

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

TargetTypeHierarchy::TargetTypeHierarchy(
    const char* name,
    const Scope& scope,
    const DexType* root
) : name(name) {
  auto class_hierarchy = build_type_hierarchy(scope);
  collect_classes(class_hierarchy, root, model_classes);
  collect_interfaces(model_classes, interfaces);
}

TargetTypeHierarchy::TargetTypeHierarchy(
    const char* name,
    const TargetTypeHierarchy& left,
    const TargetTypeHierarchy& right)
        : name(name) {
  model_classes.insert(left.model_classes.begin(), left.model_classes.end());
  model_classes.insert(right.model_classes.begin(), right.model_classes.end());
  collect_interfaces(model_classes, interfaces);
}

TargetTypeHierarchy TargetTypeHierarchy::build_target_type_hierarchy(
    const Scope& scope) {
  TargetTypeHierarchy tree_model("Tree", scope,
    DexType::make_type("Lcom/facebook/graphservice/Tree;")
  );
  TargetTypeHierarchy base_model("Base", scope,
    DexType::make_type("Lcom/facebook/graphql/modelutil/BaseModel;")
  );
  TargetTypeHierarchy gql_model("Every", tree_model, base_model);

  tree_model.print();
  base_model.print();
  gql_model.print();
  return gql_model;
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
