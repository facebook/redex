/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "VirtualRenamer.h"
#include "DexClass.h"
#include "VirtualScope.h"
#include "DexUtil.h"
#include "DexAccess.h"
#include "Trace.h"

#include <map>
#include <set>


namespace {

struct unrenamable_counters {
  size_t not_subclass_object{0};
  size_t escaped_all_mark{0};
  size_t escaped_single_mark{0};
  size_t escaped_intf{0};
  size_t escaped_override{0};
  size_t external_method{0};
  size_t object_methods{0};
};
static unrenamable_counters s_ctr;

/**
 * For anything not method_map, simply concatenates the maps/sets (we want
 * just one map for each at the end). For method_map, concatenates all the
 * corresponding vectors of wrappers.
 */
void merge(MethodLinkManager& links, MethodLinkManager& child_links) {
  for (auto& meth_typeset : child_links.class_interfaces)
    links.class_interfaces[meth_typeset.first].insert(
      meth_typeset.second.begin(), meth_typeset.second.end());
  for (auto& name_impls : child_links.interface_methods)
    for (auto& proto_set : name_impls.second)
      links.interface_methods[name_impls.first][proto_set.first].insert(
        proto_set.second.begin(), proto_set.second.end());
  for (auto& cls_set : child_links.class_conflict_set)
    links.class_conflict_set[cls_set.first] = cls_set.second;
  for (auto& name_map : child_links.method_map) {
    DexString* name = name_map.first;
    for (auto& proto_methods : name_map.second) {
      DexProto* proto = proto_methods.first;
      auto& wrapvec(links.method_map[name][proto]);
      auto& childwrapvec(child_links.method_map[name][proto]);
      wrapvec.insert(wrapvec.end(),
          std::make_move_iterator(childwrapvec.begin()),
          std::make_move_iterator(childwrapvec.end()));
    }
  }
}

// Marks all methods in the current manager as do not rename
void mark_all_escaped(MethodLinkManager& links) {
  for (auto& name_map : links.method_map) {
    for (auto& proto_methods : name_map.second) {
      for (MethodNameWrapper* wrap : proto_methods.second) {
        if (wrap->should_rename()) {
          s_ctr.escaped_all_mark += wrap->get_num_links();
          TRACE(OBFUSCATE, 1, "Marking all unrenamable");
          wrap->mark_unrenamable();
        }
      }
    }
  }
}

bool load_interfaces_methods_link(const std::deque<DexType*>&,
    MethodLinkManager&, std::unordered_set<const DexType*>&);

/**
 * Load methods for a given interface and its super interfaces.
 * Return true if any interface escapes (no DexClass*).
 */
bool load_interface_methods_link(
    const DexClass* intf_cls, MethodLinkManager& links,
    std::unordered_set<const DexType*>& class_interfaces) {
  TRACE(OBFUSCATE, 3, "\tIntf: %s\n", SHOW(intf_cls));
  bool escaped = false;
  const auto& interfaces = intf_cls->get_interfaces()->get_type_list();
  if (interfaces.size() > 0) {
    if (load_interfaces_methods_link(interfaces, links, class_interfaces))
      escaped = true;
  }
  for (const auto& meth : get_vmethods(intf_cls->get_type())) {
    links.interface_methods[meth->get_name()][meth->get_proto()].insert(meth);
  }
  return escaped;
}

/**
 * Load methods for a list of interfaces.
 * If any interface escapes (no DexClass*) return true.
 * also record all the interfaces we hit in the set of interfaces associated
 * with the class in links.class_interfaces
 */
bool load_interfaces_methods_link(
    const std::deque<DexType*>& interfaces, MethodLinkManager& links,
    std::unordered_set<const DexType*>& class_interfaces) {
  bool escaped = false;
  for (const auto& intf : interfaces) {
    class_interfaces.insert(intf);
    auto intf_cls = type_class(intf);
    if (intf_cls == nullptr) {
      escaped = true;
      continue;
    }
    if (load_interface_methods_link(intf_cls, links, class_interfaces))
      escaped = true;
  }
  return escaped;
}

bool get_interface_methods_link(const DexType* type, MethodLinkManager& links) {
  auto cls = type_class(type);
  if (cls == nullptr) return false;
  bool escaped = false;
  std::unordered_set<const DexType*>& class_interfaces(
      links.class_interfaces[type]);
  const auto& interfaces = cls->get_interfaces()->get_type_list();
  if (interfaces.size() > 0) {
    if (load_interfaces_methods_link(interfaces, links, class_interfaces))
      escaped = true;
  }
  return escaped;
}

void link_parent_children_methods(
    const DexType* parent, bool escape,
    MethodLinkManager& links, DexMethodManager& name_manager) {
  auto vmethods = get_vmethods(parent);
  for (auto& vmeth : vmethods) {
    MethodNameWrapper* method =
        static_cast<MethodNameWrapper*>(name_manager[vmeth]);
    if (escape) {
      if (method->should_rename()) {
        s_ctr.escaped_single_mark += method->get_num_links();
        TRACE(OBFUSCATE, 3, "Parent %s unrenamable\n", SHOW(method->get()));
        method->mark_unrenamable();
      }
    } else {
      // Deal with interface implementations
      auto proto_set_by_name =
          links.interface_methods.find(vmeth->get_name());
      if (proto_set_by_name != links.interface_methods.end()) {
        auto meth_set_by_proto =
            proto_set_by_name->second.find(vmeth->get_proto());
        if (meth_set_by_proto != proto_set_by_name->second.end()) {
          for (DexMethod* intf_meth : meth_set_by_proto->second) {
            auto meth =
                static_cast<MethodNameWrapper*>(name_manager[intf_meth]);
            if (meth->should_rename() && !method->should_rename()) {
              TRACE(OBFUSCATE, 2, "3: %s preventing %s from being renamed\n",
                  SHOW(method->get()), SHOW(meth->get()));
              s_ctr.escaped_intf += method->get_num_links();
            }
            if (!meth->should_rename() && method->should_rename()) {
              TRACE(OBFUSCATE, 2, "3: %s preventing %s from being renamed\n",
                  SHOW(meth->get()), SHOW(method->get()));
              s_ctr.escaped_intf += meth->get_num_links();
            }
            method->link(meth);
          }
        }
      }
    }
    // Here we link overrides
    auto& meths_by_name = links.method_map[vmeth->get_name()];
    auto& meths_by_proto = meths_by_name[vmeth->get_proto()];
    if (meths_by_proto.size() != 0) {
      // we have seen the method already -- the list is all the overrides for
      // this method
      for (auto& meth : meths_by_proto) {
        if (meth->should_rename() && !method->should_rename()) {
          TRACE(OBFUSCATE, 2, "4: %s preventing %s from being renamed\n",
              SHOW(method->get()), SHOW(meth->get()));
          s_ctr.escaped_override += method->get_num_links();
        }
        if (!meth->should_rename() && method->should_rename()) {
          TRACE(OBFUSCATE, 2, "4: %s preventing %s from being renamed\n",
              SHOW(meth->get()), SHOW(method->get()));
          s_ctr.escaped_override += meth->get_num_links();
        }
        meth->link(method);
      }
      // replace all entries now by this entry since they're all linked already
      meths_by_proto.clear();
    }
    if (method->should_rename()) {
      if (type_class(parent) != nullptr && type_class(parent)->is_external()) {
        s_ctr.external_method += method->get_num_links();
        TRACE(OBFUSCATE,
              2,
              "Marking element of external class unrenamable %s\n",
              SHOW(method->get()));
        method->mark_unrenamable();
      }
    }
    // add current method to list of methods for that name and sig
    meths_by_proto.push_back(method);
  }
}

}

VirtualRenamer::VirtualRenamer(const Scope& scope) :
    m_scope(scope), name_manager(new_dex_method_manager()) {
  class_hierarchy = build_type_hierarchy(scope);
  methods = build_signature_map(class_hierarchy);
}

bool VirtualRenamer::link_methods_helper(
    const DexType* parent, const TypeSet& children, MethodLinkManager& links) {
  bool escape = false;
  // recurse through every child in a BFS style to collect all methods
  // and interface methods under parent
  // Update conflict set with methods from parent class
    // on the way down we should never encounter something unrenamable
  const auto& meths = get_vmethods(parent);
  for (const auto& m : meths) {
    if (!is_private(m)) {
      links.parent_conflict_set.insert(m);
    }
  }
  links.class_conflict_set[parent] = links.parent_conflict_set;
  MethodLinkManager parent_intf_methods;
  bool escape_intf = get_interface_methods_link(parent, parent_intf_methods);
  auto& parent_intfs(parent_intf_methods.class_interfaces[parent]);
  // Have to pass down interface information otherwise interface implementations
  // won't correctly be linked
  for (const auto& child : children) {
    MethodLinkManager child_links;
    merge(child_links, parent_intf_methods);
    child_links.parent_conflict_set = links.parent_conflict_set;
    child_links.class_interfaces[child].insert(
      parent_intfs.begin(), parent_intfs.end());
    TRACE(OBFUSCATE,
          2,
          "%s intfs %d %s child intfs %d\n",
          SHOW(parent),
          parent_intfs.size(),
          SHOW(child),
          child_links.class_interfaces[child].size());
    escape =
        link_methods_helper(child, class_hierarchy[child], child_links) ||
        escape;
    merge(links, child_links);
    links.class_conflict_set[parent].insert(
        child_links.class_conflict_set[child].begin(),
        child_links.class_conflict_set[child].end());
  }

  merge(links, parent_intf_methods);

  // get parent interface methods
  /*TRACE(OBFUSCATE, 2, "Processing class %s %d intfs\n", SHOW(parent),
      links.class_interfaces[parent].size());
  DexType* cls = const_cast<DexType*>(parent);
  while (cls && type_class(cls)) {
    TRACE(OBFUSCATE, 3, "\t^^ %s\n", SHOW(cls));
    cls = type_class(cls)->get_super_class();
  }*/

  escape = escape || escape_intf;

  link_parent_children_methods(
      parent, escape, links, name_manager);
  if (escape_intf) {
    // if any interface in parent escapes, mark all children methods 'impl'
    mark_all_escaped(links);
  }

  return escape;
}

void VirtualRenamer::mark_methods_renamable(const DexType* cls) {
  if (type_class(cls) == nullptr) return;
  if (!type_class(cls)->is_external()) {
    for (const auto& meth : get_vmethods(cls)) {
      s_ctr.not_subclass_object--;
      name_manager[const_cast<DexMethod*>(meth)]->mark_renamable();
    }
  }
  for (auto& child : class_hierarchy[cls])
    mark_methods_renamable(child);
}


MethodLinkInfo VirtualRenamer::link_methods() {
  class_hierarchy = build_type_hierarchy(m_scope);

  auto object = get_object_type();
  auto& children = class_hierarchy[object];

  MethodLinkManager links;
  link_methods_helper(object, children, links);

  // Make sure anything that is a method of java.lang.Object is not renamable
  for (auto meth : get_vmethods(get_object_type())) {
    if (name_manager[meth]->should_rename()) {
      s_ctr.object_methods++;
      TRACE(OBFUSCATE,
            2,
            "Marking method of object %s unrenamable\n",
            SHOW(meth));
      name_manager[meth]->mark_unrenamable();
    }
  }

  // Create the value we're returning
  MethodLinkInfo res(links.class_interfaces, name_manager);
  // build reverse of class_interfaces
  std::unordered_map<const DexType*, std::unordered_set<const DexType*>>
      interface_classes;
  for (auto& cls_intfs : res.class_interfaces)
    for (auto& intf : cls_intfs.second)
      interface_classes[intf].insert(cls_intfs.first);
  // build intf_conflict_set
  for (auto& intf_classes : interface_classes) {
    for (auto& cls : intf_classes.second) {
      res.intf_conflict_set[intf_classes.first].insert(
          links.class_conflict_set[cls].begin(),
          links.class_conflict_set[cls].end());
    }
  }

  TRACE(OBFUSCATE, 3, "Returned conflict sets:\n");
  for (auto class_confset : res.intf_conflict_set) {
    TRACE(OBFUSCATE, 3, "\t%s:\n", SHOW(class_confset.first));
    for (auto intf : class_confset.second)
      TRACE(OBFUSCATE, 3, "\t\t%s\n", SHOW(intf));
  }
  TRACE(
      OBFUSCATE,
      3,
      "not_subclass_object %d\n object_methods %d\n escaped_cls"
      " (all mark) %d\n escaped_cls (single mark) %d\n escaped_cls (intf) %d\n "
      "escaped_cls (override) %d\n external class method %d\n",
      s_ctr.not_subclass_object,
      s_ctr.object_methods,
      s_ctr.escaped_all_mark,
      s_ctr.escaped_single_mark,
      s_ctr.escaped_intf,
      s_ctr.escaped_override,
      s_ctr.external_method);

  return res;
}

MethodLinkInfo link_methods(Scope& scope) {
  VirtualRenamer devirtualizer(scope);
  std::unordered_set<DexMethod*> final_meths;
  auto link_res = devirtualizer.link_methods();

  return link_res;
}
