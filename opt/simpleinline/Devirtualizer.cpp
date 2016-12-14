/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Devirtualizer.h"
#include "DexUtil.h"
#include "DexAccess.h"
#include "Trace.h"

#include <map>
#include <set>

#include <iostream>
#include <fstream>

namespace {

/**
 * Flags to mark virtual method status.
 */
enum MethodAccess : uint16_t {
  // the first method definition in a hierarchy tree
  TOP_DEF = 0x0,
  // the method overrides one up the hierarchy chain
  OVERRIDE = 0X1,
  // the method is an implementation of an interface
  IMPL = 0X2,
  // the method is final
  FINAL = 0X4,
};

inline MethodAccess operator|=(MethodAccess& a, const MethodAccess b) {
  return (a = static_cast<MethodAccess>(static_cast<uint16_t>(a) |
                                        static_cast<uint16_t>(b)));
}

using ClassSet = std::unordered_set<DexClass*>;
using TypeSet = std::unordered_set<const DexType*>;
using ProtoSet = std::unordered_set<DexProto*>;

using MethAcc = std::pair<DexMethod*, MethodAccess>;

using ClassHierarchy = std::map<const DexType*, TypeSet>;
using MethodsSigMap = std::unordered_map<DexProto*, std::vector<MethAcc>>;
using MethodsNameMap = std::unordered_map<DexString*, MethodsSigMap>;
using InterfaceMethods = std::unordered_map<DexString*, ProtoSet>;



std::ofstream logfile;
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
/*
void log_mark_methods(const DexType* parent, const TypeSet& children) {
  logfile << "mark methods for " << SHOW(parent) << " with children ";
  for (const auto& child : children) {
    logfile << SHOW(child) << ", ";
  }
  logfile << "\n";
}

void log_interface_methods(const char* msg, InterfaceMethods& methods) {
  logfile << msg << methods.size() << "\n";
  for (const auto& i_m_it : methods) {
    logfile << SHOW(i_m_it.first) << ": ";
    for (const auto& proto : i_m_it.second) {
      logfile << SHOW(proto) << ", ";
    }
    logfile << "\n";
  }
}

void log_methods_name_map(const char* msg, MethodsNameMap& methods) {
  logfile << msg << methods.size() << "\n";
  for (const auto& meth_it : methods) {
    logfile << SHOW(meth_it.first) << ": ";
    for (const auto& msig_it : meth_it.second) {
      logfile << SHOW(msig_it.first) << "[";
      for (const auto& m_acc_it : msig_it.second) {
        logfile << SHOW(m_acc_it.first->get_class())
            << "." << SHOW(m_acc_it.first->get_name())
            << ": " << SHOW(m_acc_it.first->get_proto()) << ", ";
      }
      logfile << "]";
    }
    logfile << "\n";
  }
}

void log_final_results(MethodsNameMap& methods) {
  logfile << "final result\n";
  for (const auto& meths_by_name : methods) {
    for (const auto& meths_by_sig : meths_by_name.second) {
      for (const auto& meths : meths_by_sig.second) {
        if (!meths.first->is_concrete()) {
          logfile << "non concrete: "
              << SHOW(meths.first->get_class())
              << "."
              << SHOW(meths.first->get_name())
              << ": "
              << SHOW(meths.first->get_proto())
              << "\n";
          continue;
        }
        if (meths.second == FINAL) {
          logfile << "non virtual: "
              << SHOW(meths.first->get_class())
              << "."
              << SHOW(meths.first->get_name())
              << ": "
              << SHOW(meths.first->get_proto())
              << "\n";
        } else {
          logfile << "non final: "
              << SHOW(meths.first->get_class())
              << "."
              << SHOW(meths.first->get_name())
              << ": "
              << SHOW(meths.first->get_proto())
              << "\n";
        }
      }
    }
  }
}
*/

/**
 * Return the list of methods for a given type.
 * If the type is java.lang.Object and it is not known (no DexClass for it)
 * it generates fictional methods for it.
 */
std::list<DexMethod*> get_vmethods(DexType* type) {
  const DexClass* cls = type_class(type);
  if (cls != nullptr) return cls->get_vmethods();
  always_assert_log(type == get_object_type(), "Unknown type %s\n", SHOW(type));
  std::list<DexMethod*> object_methods;

  // create the following methods:
  // protected java.lang.Object.clone()Ljava/lang/Object;
  // public java.lang.Object.equals(Ljava/lang/Object;)Z
  // protected java.lang.Object.finalize()V
  // public final native java.lang.Object.getClass()Ljava/lang/Class;
  // public native java.lang.Object.hashCode()I
  // public final native java.lang.Object.notify()V
  // public final native java.lang.Object.notifyAll()V
  // public java.lang.Object.toString()Ljava/lang/String;
  // public final java.lang.Object.wait()V
  // public final java.lang.Object.wait(J)V
  // public final native java.lang.Object.wait(JI)V

  // required sigs
  auto void_args = DexTypeList::make_type_list({});
  auto void_object = DexProto::make_proto(get_object_type(), void_args);
  auto object_bool = DexProto::make_proto(
      get_boolean_type(), DexTypeList::make_type_list({get_object_type()}));
  auto void_void = DexProto::make_proto(get_void_type(), void_args);
  auto void_class = DexProto::make_proto(get_class_type(), void_args);
  auto void_int = DexProto::make_proto(get_int_type(), void_args);
  auto void_string = DexProto::make_proto(get_string_type(), void_args);
  auto long_void = DexProto::make_proto(
      get_void_type(), DexTypeList::make_type_list({get_int_type()}));
  auto long_int_void = DexProto::make_proto(
      get_void_type(),
      DexTypeList::make_type_list({get_long_type(), get_int_type()}));

  // required names
  auto clone = DexString::make_string("clone");
  auto equals = DexString::make_string("equals");
  auto finalize = DexString::make_string("finalize");
  auto getClass = DexString::make_string("getClass");
  auto hashCode = DexString::make_string("hashCode");
  auto notify = DexString::make_string("notify");
  auto notifyAll = DexString::make_string("notifyAll");
  auto toString = DexString::make_string("toString");
  auto wait = DexString::make_string("wait");

  // create methods and add to the list of object methods
  // All the checks to see if the methods exist are because we cannot set
  // access/virtual for external methods, so if the method exists (i.e. if this
  // function is called multiple times with get_object_type()), we will fail
  // an assertion. This only happens in tests when no external jars are
  // available
  // protected java.lang.Object.clone()Ljava/lang/Object;
  DexMethod* method = DexMethod::get_method(type, clone, void_object);
  if (method == nullptr) {
    method = DexMethod::make_method(type, clone, void_object);
    method->set_access(ACC_PROTECTED);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);

  // public java.lang.Object.equals(Ljava/lang/Object;)Z
  method = DexMethod::get_method(type, equals, object_bool);
  if (method == nullptr) {
    method = DexMethod::make_method(type, equals, object_bool);
    method->set_access(ACC_PUBLIC);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, finalize, void_void);
  if (method == nullptr) {
    // protected java.lang.Object.finalize()V
    method = DexMethod::make_method(type, finalize, void_void);
    method->set_access(ACC_PROTECTED);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, getClass, void_class);
  if (method == nullptr) {
    // public final native java.lang.Object.getClass()Ljava/lang/Class;
    method = DexMethod::make_method(type, getClass, void_class);
    method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, hashCode, void_int);
  if (method == nullptr) {
    // public native java.lang.Object.hashCode()I
    method = DexMethod::make_method(type, hashCode, void_int);
    method->set_access(ACC_PUBLIC | ACC_NATIVE);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, notify, void_void);
  if (method == nullptr) {
    // public final native java.lang.Object.notify()V
    method = DexMethod::make_method(type, notify, void_void);
    method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, notifyAll, void_void);
  if (method == nullptr) {
    // public final native java.lang.Object.notifyAll()V
    method = DexMethod::make_method(type, notifyAll, void_void);
    method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, toString, void_string);
  if (method == nullptr) {
    // public java.lang.Object.toString()Ljava/lang/String;
    method = DexMethod::make_method(type, toString, void_string);
    method->set_access(ACC_PUBLIC);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, wait, void_void);
  if (method == nullptr) {
    // public final java.lang.Object.wait()V
    method = DexMethod::make_method(type, wait, void_void);
    method->set_access(ACC_PUBLIC | ACC_FINAL);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, wait, long_void);
  if (method == nullptr) {
    // public final java.lang.Object.wait(J)V
    method = DexMethod::make_method(type, wait, long_void);
    method->set_access(ACC_PUBLIC | ACC_FINAL);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);
  method = DexMethod::get_method(type, wait, long_int_void);
  if (method == nullptr) {
    // public final native java.lang.Object.wait(JI)V
    method = DexMethod::make_method(type, wait, long_int_void);
    method->set_access(ACC_PUBLIC | ACC_FINAL | ACC_NATIVE);
    method->set_virtual(true);
    method->set_external();
  }
  object_methods.push_back(method);

  return object_methods;
}

/**
 * Merge the methods map in derived with that of base for the given type.
 */
void merge(MethodsNameMap& base, MethodsNameMap& derived) {
  for (const auto& entry : derived) {
    const auto& base_entry = base.find(entry.first);
    if (base_entry == base.end()) {
      base[entry.first] = entry.second;
      continue;
    }
    auto& sig_to_meths = base_entry->second;
    for (const auto& sigs : entry.second) {
      auto& meths = sig_to_meths[sigs.first];
      meths.insert(meths.end(), sigs.second.begin(), sigs.second.end());
    }
  }
}

/**
 * Merge the interface methods map in derived with that of base.
 */
void merge(
    InterfaceMethods& intf_methods, InterfaceMethods& child_intf_methods) {
  // TODO: remove after we wrote enough unit tests to make sure all cases
  //       are covered
  // intf_methods.insert(child_intf_methods.begin(), child_intf_methods.end());
  for (const auto& child_meth_it : child_intf_methods) {
    const auto& intf_protos = intf_methods.find(child_meth_it.first);
    if (intf_protos == intf_methods.end()) {
      intf_methods[child_meth_it.first] = child_meth_it.second;
      continue;
    }
    auto& proto_set = intf_protos->second;
    proto_set.insert(child_meth_it.second.begin(), child_meth_it.second.end());
  }
}

//
// Helpers to load interface methods in a MethodMap.
//

bool load_interfaces_methods(const std::list<DexType*>&, InterfaceMethods&);

/**
 * Load methods for a given interface and its super interfaces.
 * Return true if any interface escapes (no DexClass*).
 */
bool load_interface_methods(const DexClass* intf_cls,
                            InterfaceMethods& methods) {
  bool escaped = false;
  const auto& interfaces = intf_cls->get_interfaces()->get_type_list();
  if (interfaces.size() > 0) {
    if (load_interfaces_methods(interfaces, methods)) escaped = true;
  }
  for (const auto& meth : intf_cls->get_vmethods()) {
    methods[meth->get_name()].insert(meth->get_proto());
  }
  return escaped;
}

/**
 * Load methods for a list of interfaces.
 * If any interface escapes (no DexClass*) return true.
 */
bool load_interfaces_methods(
    const std::list<DexType*>& interfaces, InterfaceMethods& methods) {
  bool escaped = false;
  for (const auto& intf : interfaces) {
    auto intf_cls = type_class(intf);
    if (intf_cls == nullptr) {
      escaped = true;
      continue;
    }
    if (load_interface_methods(intf_cls, methods)) escaped = true;
    // log_interface_methods("Load Interface Methods: ", methods);
  }
  return escaped;
}

/**
 * Get all interface methods for a given type.
 */
bool get_interface_methods(const DexType* type, InterfaceMethods& methods) {
  auto cls = type_class(type);
  if (cls == nullptr) return false;
  bool escaped = false;
  const auto& interfaces = cls->get_interfaces()->get_type_list();
  if (interfaces.size() > 0) {
    if (load_interfaces_methods(interfaces, methods)) escaped = true;
  }
  return escaped;
}

/**
* Mark all methods in map as 'impl'.
* This happens when an interface on some class is unknown and so we have
* no way to tell if any of the children methods is an implementation of
* that interface. So we conservatively mark every children method 'impl'.
*/
void impl_all(MethodsNameMap& methods) {
  for (auto& meths_by_name : methods) {
    for (auto& meths_by_proto : meths_by_name.second) {
      for (auto& meth : meths_by_proto.second) {
        meth.second |= IMPL;
      }
    }
  }
}

/**
* Given a set of interface methods from a parent, mark all children methods
* that match as 'impl'.
*/
void impl_intf_methods(
    MethodsNameMap& methods, InterfaceMethods& intf_methods) {
  for (const auto& intf_meths_by_name : intf_methods) {
    const auto& meths_by_name = methods.find(intf_meths_by_name.first);
    if (meths_by_name == methods.end()) continue;
    for (const auto& intf_meth_sig : intf_meths_by_name.second) {
      const auto& meths_by_sig = meths_by_name->second.find(intf_meth_sig);
      if (meths_by_sig == meths_by_name->second.end()) continue;
      for (auto& meth : meths_by_sig->second) {
        meth.second |= IMPL;
      }
    }
  }
}

/**
 * Mark all children methods according to parent and children interfaces:
 * - for every method in parent:
 *    - if escape is true mark it 'impl'
 *    - if matches a method in children interfaces mark it 'impl'
 *    - if method not in children methods mark it 'final'
 *    - otherwise leave it as is
 * - for every method in children that matches name and sig of a method
 * in parent, mark it 'override'
 */
void analyze_parent_children_methods(
    const DexType* parent, MethodsNameMap& children_methods,
    InterfaceMethods& children_intf_methods, bool escape) {
  auto const& vmethods = get_vmethods(const_cast<DexType*>(parent));
  for (auto& vmeth : vmethods) {
    auto meth_acc = std::make_pair(vmeth, TOP_DEF);
    if (escape) meth_acc.second |= IMPL;
    else {
      const auto& intf_meth_by_name =
          children_intf_methods.find(vmeth->get_name());
      if (intf_meth_by_name != children_intf_methods.end() &&
          intf_meth_by_name->second.count(vmeth->get_proto()) > 0) {
        meth_acc.second |= IMPL;
      }
    }
    auto& meths_by_name = children_methods[vmeth->get_name()];
    auto& meths_by_proto = meths_by_name[vmeth->get_proto()];
    if (meths_by_proto.size() == 0) {
      // first time we see this method, make it final if not in escape mode
      meth_acc.second |= FINAL;
    } else {
      // we have seen the method already mark all seen override
      for (auto& meth : meths_by_proto) {
        meth.second |= OVERRIDE;
      }
    }
    // add current method to list of methods for that name and sig
    meths_by_proto.push_back(meth_acc);
  }
}

// ======= Method Linking Code ===========

class MethodLinkManager {
public:
  // class -> interfaces implemented in its hierarchy
  std::unordered_map<const DexType*,
      std::unordered_set<const DexType*>> class_interfaces;
  // methods that implement an interface
  std::unordered_map<DexString*,
      std::unordered_map<DexProto*,
          std::unordered_set<DexMethod*>>> interface_methods;
  // class -> set of all public methods in its hierarchy
  std::unordered_map<const DexType*,
      std::unordered_set<DexMethod*>> class_conflict_set;
  // for recursion - set of all public methods in any superclasses
  std::unordered_set<DexMethod*> parent_conflict_set;
  // index with name, proto
  // reason for the vector: we might have two elements with the same signature
  // as we merge the maps going up the tree, but we don't necessarily want
  // to link these elements
  std::unordered_map<
      DexString*,
      std::unordered_map<DexProto*, std::vector<MethodNameWrapper*>>>
      method_map;

  MethodLinkManager() {}
};

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


bool load_interfaces_methods_link(const std::list<DexType*>&,
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
  for (const auto& meth :
       get_vmethods(const_cast<DexType*>(intf_cls->get_type()))) {
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
    const std::list<DexType*>& interfaces, MethodLinkManager& links,
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
  auto vmethods = get_vmethods(const_cast<DexType*>(parent));
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

/**
 * Perform devirtualization by building the type hierarchy and identifying all
 * methods that do not need to be virtual.
 */
class Devirtualizer {
 public:
  Devirtualizer(std::vector<DexClass*>& scope) :
      name_manager(new_dex_method_manager()), m_scope(scope) {}

  std::vector<DexMethod*> devirtualize();

  MethodLinkInfo link_methods();

 private:
  void build_class_hierarchy(DexClass* cls);
  void analyze_methods(std::vector<DexMethod*>& non_virtual);
  bool mark_methods(
      const DexType* parent, const TypeSet& children,
      InterfaceMethods& intf_methods, MethodsNameMap& methods);
  bool link_methods_helper(
      const DexType* parent, const TypeSet& children,
      MethodLinkManager& links);
  void mark_methods_renamable(const DexType* cls);

private:
  DexMethodManager name_manager;
  const std::vector<DexClass*>& m_scope;
  ClassHierarchy m_class_hierarchy;
};

std::vector<DexMethod*> Devirtualizer::devirtualize() {
  std::vector<DexMethod*> non_virtual;
  for (const auto& cls : m_scope) {
    if (is_interface(cls) || is_annotation(cls)) continue;
    build_class_hierarchy(cls);
  }
  analyze_methods(non_virtual);
  return non_virtual;
}

bool Devirtualizer::link_methods_helper(
    const DexType* parent, const TypeSet& children, MethodLinkManager& links) {
  bool escape = false;
  // recurse through every child in a BFS style to collect all methods
  // and interface methods under parent
  // Update conflict set with methods from parent class
    // on the way down we should never encounter something unrenamable
  const auto& meths = get_vmethods(const_cast<DexType*>(parent));
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
        link_methods_helper(child, m_class_hierarchy[child], child_links) ||
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

void Devirtualizer::mark_methods_renamable(const DexType* cls) {
  if (type_class(cls) == nullptr) return;
  if (!type_class(cls)->is_external()) {
    for (const auto& meth : get_vmethods(const_cast<DexType*>(cls))) {
      s_ctr.not_subclass_object--;
      name_manager[const_cast<DexMethod*>(meth)]->mark_renamable();
    }
  }
  for (auto& child : m_class_hierarchy[cls])
    mark_methods_renamable(child);
}


MethodLinkInfo Devirtualizer::link_methods() {
  for (const auto& cls : m_scope) {
    if (is_interface(cls) || is_annotation(cls)) continue;
    build_class_hierarchy(cls);
  }

  auto object = get_object_type();
  auto& children = m_class_hierarchy[object];

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

/**
 * Given a class walks up the hierarchy and creates entries from parent to
 * children.
 * If no super is found the type is considered a child of java.lang.Object.
 * If the type is unknown (no DexClass) the walk stops and the hierarchy is
 * formed up to the first unknown type.
 */
void Devirtualizer::build_class_hierarchy(DexClass* cls) {
  // ensure an entry for the DexClass is created
  m_class_hierarchy[cls->get_type()];
  while (cls != nullptr) {
    auto type = cls->get_type();
    const auto super = cls->get_super_class();
    if (super != nullptr) {
      m_class_hierarchy[super].insert(type);
    } else {
      if (type != get_object_type()) {
        // if the type in question is not java.lang.Object and it has
        // no super make it a subclass of java.lang.Object
        m_class_hierarchy[get_object_type()].insert(type);
        TRACE(SINL, 4, "no super on %s\n", SHOW(type));
      }
    }
    cls = type_class(super);
  }
}

/**
 * Walk the java.lang.Object class hierarchy and build a map of
 * name => sig => methods for each subtree. Then mark each method in the
 * subtree according to the parent's methods.
 * The walk is bottom up (children to parent) then
 * - methods seen for the first time are marked final
 * - otherwise the list of methods for the given name and signatures are
 * marked override
 * - method implementing an interface are marked impl
 */
void Devirtualizer::analyze_methods(std::vector<DexMethod*>& non_virtual) {
  auto object = get_object_type();
  const auto& children = m_class_hierarchy[object];
  MethodsNameMap methods;
  InterfaceMethods intf_methods;
  mark_methods(object, children, intf_methods, methods);

  // log_final_results(methods);
  for (const auto& meths_by_name : methods) {
    for (const auto& meths_by_sig : meths_by_name.second) {
      for (const auto& meths : meths_by_sig.second) {
        if (!meths.first->is_concrete()) continue;
        if (meths.second == FINAL) non_virtual.push_back(meths.first);
      }
    }
  }
}

/**
 * Compute methods FINAL, OVERRIDE and IMPL properties.
 * Starting from java.lang.Object recursively walk the type hierarchy down
 * BFS style and while unwinding compare each method in the class being
 * traversed with all methods coming from the children.
 * Then perform the following:
 * 1- if a method in the parent does not exist in any children mark it FINAL
 * 2- if a method in the parent matches a list of methods in the children
 * mark all children OVERRIDE
 * 3- if a method is an implementation of an interface method mark it IMPL
 *
 * At the end top methods (where the method is introduced) are the only
 * non OVERRIDE and possibly non IMPL.
 * Any method that is FINAL and not OVERRIDE or IMPL is effectively a
 * non virtual.
 * Interfaces add a painful spin to this, best expressed by examples:
 * class A { void m() {} }
 * interface I { void m(); }
 * class B extends A implements I {}
 * in this case A.m() must be marked IMPL even though is up in the hierarchy
 * chain. If not, it would be a FINAL non OVERRIDE and could be inlined and
 * deleted breaking the interface contract. So we mark all methods that
 * match interface down the hierarchy as IMPL.
 * If an interface is not known (escapes) we mark all children methods
 * and all methods up the hierarchy chain IMPL.
 * Consider this example and assume interface I is unknown
 * class A { public m() {} public g() {} public f() {} }
 * class B extends A implements I {}
 * class C extends B { public void k() {} }
 * class D extends A { public void k() {} }
 * in this case, not knowing interface I, we mark all methods in A, B and C
 * IMPL but methods in D are not, so in this case they are just FINAL and
 * effectively D.k() would be non virtual as opposed to C.k() which is IMPL.
 */
bool Devirtualizer::mark_methods(
      const DexType* parent, const TypeSet& children,
      InterfaceMethods& intf_methods, MethodsNameMap& methods) {
  always_assert_log(intf_methods.size() == 0 && methods.size() == 0,
      "intf_methods and children_methods are out params");
  bool escape = false;
  // log_mark_methods(parent, children);
  // recurse through every child in a BFS style to collect all methods
  // and interface methods under parent
  for (const auto& child : children) {
    MethodsNameMap child_methods;
    InterfaceMethods child_intf_methods;
    escape = mark_methods(child, m_class_hierarchy[child],
        child_intf_methods, child_methods) || escape;
    // log_methods_name_map("Child methods: ", child_methods);
    // log_interface_methods("Child interface methods: ", child_intf_methods);
    merge(methods, child_methods);
    merge(intf_methods, child_intf_methods);
    // log_interface_methods("Interface methods after merge: ", intf_methods);
  }
  // get parent interface methods
  InterfaceMethods parent_intf_methods;
  bool escape_intf = get_interface_methods(parent, parent_intf_methods);
  // log_interface_methods("parent interface methods found: ",
  //     parent_intf_methods);
  merge(intf_methods, parent_intf_methods);
  // log_interface_methods("interface methods after merge with parent interface: ",
  //     intf_methods);

  escape = escape || escape_intf;

  analyze_parent_children_methods(parent, methods, intf_methods, escape);

  if (escape_intf) {
    // if any interface in parent escapes, mark all children methods 'impl'
    impl_all(methods);
  } else {
    impl_intf_methods(methods, parent_intf_methods);
  }

  return escape;
}

}

std::vector<DexMethod*> devirtualize(std::vector<DexClass*>& scope) {
  // logfile.open("devirtualizer.log");
  // logfile << "devirtualizing " << scope.size() << " classes\n";
  Devirtualizer devirtualizer(scope);
  auto meths = devirtualizer.devirtualize();
  // logfile.close();
  return meths;
}

MethodLinkInfo link_methods(std::vector<DexClass*>& scope) {
  Devirtualizer devirtualizer(scope);
  std::unordered_set<DexMethod*> final_meths;
  auto link_res = devirtualizer.link_methods();

  return link_res;
}
