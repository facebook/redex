/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MergeInterface.h"

#include "ClassHierarchy.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IROpcode.h"
#include "Resolver.h"
#include "Trace.h"
#include "TypeReference.h"
#include "TypeSystem.h"
#include "Walkers.h"

namespace {

using DexClassSet = std::set<DexClass*, dexclasses_comparator>;

// List of classes to List of interfaces they implemented
using ImplementorsToInterfaces = std::map<TypeSet, DexClassSet>;

std::string show(const ImplementorsToInterfaces& input) {
  std::ostringstream ss;
  ss << "============ interface and class map ============\n";
  for (const auto& pair : input) {
    ss << "classes: \n";
    for (const auto& cls : pair.first) {
      ss << "   " << cls << "\n";
    }
    ss << "interfaces: \n";
    for (const auto& cls : pair.second) {
      ss << "   " << cls << "\n";
    }
  }
  return ss.str();
}

std::string show(const std::vector<DexClassSet>& to_merge) {
  std::ostringstream ss;
  ss << "\n============ Interfaces to merge ============\n";
  for (auto it = to_merge.begin(); it != to_merge.end(); ++it) {
    ss << "Interfaces to merge: \n";
    for (const auto& intf : *it) {
      ss << "   " << intf << "\n";
    }
  }
  return ss.str();
}

std::string show(const TypeSet& type_set) {
  std::ostringstream ss;
  for (auto type : type_set) {
    ss << type << ":" << SHOW(type) << " ";
  }
  ss << "\n";
  return ss.str();
}

std::vector<DexClassSet> collect_can_merge(
    const Scope& scope,
    const std::vector<std::vector<DexClass*>>& classes_groups,
    MergeInterfacePass::Metric* metric) {
  TypeSystem ts(scope);
  std::vector<DexClassSet> interface_set;
  for (const auto& classes_group : classes_groups) {
    // Build the map of interfaces and list of classes that implement
    // the interfaces
    ImplementorsToInterfaces interface_class_map;
    std::unordered_set<DexClass*> ifaces;
    // Find interfaces that are not external, can be delete, can be renamed.
    for (auto cls : classes_group) {
      if (is_interface(cls) && !cls->is_external() && can_delete(cls) &&
          can_rename_if_ignoring_blanket_keepnames(cls)) {
        ifaces.emplace(cls);
      }
    }
    for (const auto& cls : ifaces) {
      TRACE(MEINT, 7, "interfaces: %p\n", cls->get_type());
      const TypeSet& implementors = ts.get_implementors(cls->get_type());
      TRACE(MEINT, 7, "implementors : ");
      TRACE(MEINT, 7, SHOW(implementors));
      // Need to find common interfaces that implement this interface too.
      const TypeSet& intf_children = ts.get_interface_children(cls->get_type());
      TRACE(MEINT, 7, "children intfs : ");
      TRACE(MEINT, 7, SHOW(intf_children));
      TypeSet implementors_and_intfs;
      // TODO(suree404): This merge interfaces that have same implementors and
      // interface children. But if there are interfaces that have same
      // implementors, and some interfaces are super interface of another
      // interface, that might be mergeable too.
      implementors_and_intfs.insert(implementors.begin(), implementors.end());
      implementors_and_intfs.insert(intf_children.begin(), intf_children.end());
      interface_class_map[implementors_and_intfs].emplace(cls);
    }
    TRACE(MEINT, 5, SHOW(interface_class_map));

    // Collect interfaces that we need to merge.
    for (const auto& pair : interface_class_map) {
      if (pair.first.size() > 0 && pair.second.size() > 1) {
        // Consider interfaces with same set of implementors as mergeable.
        interface_set.emplace_back(pair.second);
      }
    }
  }
  // Remove interface if it is the type of an annotation.
  // TODO(suree404): Merge the interface even though it appears in annotation?
  walk::annotations(scope, [&](DexAnnotation* anno) {
    std::vector<DexType*> types_in_anno;
    anno->gather_types(types_in_anno);
    for (const auto& type : types_in_anno) {
      DexClass* type_cls = type_class(type);
      if (type_cls == nullptr) continue;
      for (auto it = interface_set.begin(); it != interface_set.end(); ++it) {
        if (it->count(type_cls) > 0) {
          it->erase(type_cls);
          ++metric->interfaces_in_annotation;
          break;
        }
      }
    }
  });
  TRACE(MEINT, 4, SHOW(interface_set));
  return interface_set;
}

/* Imitating the process of update method signature to find interfaces
 * that if merged could cause virtual method collision (for which
 * update_method_signature_type_references() would throw error).
 *
 * This method would strip out interface like the example below
 * {Interface I1, interface I2, interface I3} are candidate for merging.
 * Class A has virtual function: void do_something(I1);
 *                               void do_something(I2);
 * merging I1 and I2 would be troublesome, this method would get rid of I1 or
 * I2 from this group, leave one of I1 and I2 to merge with I3.
 *
 * TODO(suree404): Handle virtual method collision too so this
 * function can be removed.
 */
void strip_out_collision(const Scope& scope,
                         std::vector<DexClassSet>* candidates) {
  TypeSet mergeables;

  std::unordered_map<const DexType*, DexType*> intf_merge_map;
  for (auto it = candidates->begin(); it != candidates->end(); ++it) {
    const DexClassSet& intf_set = *it;
    if (intf_set.size() <= 1) {
      continue;
    }
    auto set_start = intf_set.begin();
    DexClass* merge_to_intf = *set_start;
    ++set_start;
    for (auto set_it = set_start; set_it != intf_set.end(); ++set_it) {
      DexClass* interface_to_copy = (*set_it);
      intf_merge_map[interface_to_copy->get_type()] = merge_to_intf->get_type();
    }
  }

  for (const auto& i : intf_merge_map) {
    mergeables.insert(i.first);
  }

  std::unordered_set<DexMethodRef*> fake_sets;
  std::unordered_set<const DexType*> to_delete;
  const auto fake_update = [&](DexMethod* meth) {
    DexProto* proto = meth->get_proto();
    // TODO(suree404): Only eliminate true virtual.
    if (!meth->is_virtual() ||
        !type_reference::proto_has_reference_to(proto, mergeables)) {
      return;
    }
    DexProto* new_proto =
        type_reference::update_proto_reference(proto, intf_merge_map);
    DexType* type = meth->get_class();
    DexString* name = meth->get_name();
    DexMethodRef* existing_meth = DexMethod::get_method(type, name, new_proto);
    if (existing_meth == nullptr) {
      // When there is no virtual method conflict if we are merging the
      // interfaces, then create a fake method to indicate there will be
      // such a virtual method after merging.
      // This is to make sure that we can catch conflict case no matter
      // which merger class we choose. Suppose in the example of
      // I1, I2, and I3 above, if we chose I3 as merger, then I1 I2 will be
      // merged, updating I1 to be I3 won't cause conflict, but
      // A.do_something(I3) will be a fake method existed, then updating I2
      // to be I3 will cause conflict.
      DexMethodRef* fake_new_meth =
          DexMethod::make_method(type, name, new_proto);
      TRACE(MEINT, 7, "Making fake method\n");
      TRACE(MEINT, 7, SHOW(fake_new_meth));
      TRACE(MEINT, 7, "\n");
      fake_sets.emplace(fake_new_meth);
      return;
    }
    const DexType* rtype = get_array_type_or_self(proto->get_rtype());
    if (mergeables.count(rtype) > 0) {
      to_delete.emplace(rtype);
    }
    for (const auto arg_type : proto->get_args()->get_type_list()) {
      const DexType* extracted_arg_type = get_array_type_or_self(arg_type);
      if (mergeables.count(extracted_arg_type) > 0) {
        to_delete.emplace(extracted_arg_type);
      }
    }
  };

  walk::methods(scope, fake_update);
  for (DexMethodRef* fake_method : fake_sets) {
    TRACE(MEINT, 7, "Erasing fake method\n");
    TRACE(MEINT, 7, SHOW(fake_method));
    TRACE(MEINT, 7, "\n");
    DexMethod::erase_method(fake_method);
  }

  for (const DexType* to_del : to_delete) {
    DexClass* to_del_cls = type_class(to_del);
    for (auto it = candidates->begin(); it != candidates->end(); ++it) {
      if (it->find(to_del_cls) != it->end()) {
        it->erase(to_del_cls);
        break;
      }
    }
  }
}

/**
 * Check that visibility / accessibility changes to the current method
 * won't need to change a referenced method into a virtual or static one.
 * If it does, return false.
 */
bool will_fail_relocate(DexMethod* method) {
  IRCode* code = method->get_code();
  always_assert(code);

  for (const auto& mie : InstructionIterable(code)) {
    IRInstruction* insn = mie.insn;
    if (insn->opcode() == OPCODE_INVOKE_DIRECT) {
      DexMethod* meth =
          resolve_method(insn->get_method(), MethodSearch::Direct);
      if (!meth) {
        return true;
      }

      always_assert(meth->is_def());
      if (!is_init(meth)) {
        return true;
      }
    }
  }
  return false;
}

/*
 * Remove interfaces that have dmethod that will be be changed virtual or
 * static methods when changed visibility or accessibilty relocating to merger
 * interface.
 */
void strip_out_dmethod_relo_problem_intf(const Scope& scope,
                                         std::vector<DexClassSet>* candidates) {
  std::unordered_set<DexClass*> to_delete;
  for (auto it = candidates->begin(); it != candidates->end(); ++it) {
    const DexClassSet& intf_set = *it;
    if (intf_set.size() <= 1) {
      continue;
    }
    for (auto set_it = intf_set.begin(); set_it != intf_set.end(); ++set_it) {
      DexClass* interface_cls = (*set_it);
      const auto& dmethods = interface_cls->get_dmethods();
      if (dmethods.size() == 0) {
        continue;
      }
      for (const auto& dmethod : dmethods) {
        if (will_fail_relocate(dmethod)) {
          to_delete.insert(interface_cls);
          break;
        }
      }
    }
  }
  for (DexClass* intf : to_delete) {
    TRACE(MEINT, 7, "Excluding interface %s because of dmethod relocation.\n",
          SHOW(intf->get_type()));
    for (auto it = candidates->begin(); it != candidates->end(); ++it) {
      if (it->find(intf) != it->end()) {
        it->erase(intf);
        break;
      }
    }
  }
}

void move_methods_to_interface(
    DexClass* from_interface,
    DexClass* target_interface,
    std::unordered_map<DexMethodRef*, DexMethodRef*>* old_to_new_method) {
  DexType* target_intf_type = target_interface->get_type();
  auto dmethods = from_interface->get_dmethods();
  auto vmethods = from_interface->get_vmethods();
  // Move static methods.
  for (auto it = dmethods.begin(); it != dmethods.end(); ++it) {
    DexMethod* method_to_move = *it;
    bool success_relocate =
        relocate_method_if_no_changes(method_to_move, target_intf_type);
    always_assert_log(success_relocate,
                      "Merge interface %s relocate dmethod %s failed.",
                      SHOW(from_interface), SHOW(method_to_move));
  }
  // Move virtual methods.
  for (auto it = vmethods.begin(); it != vmethods.end(); ++it) {
    DexMethod* method_to_move = *it;
    always_assert_log(method_to_move->get_code() == nullptr,
                      "Interface vmethod %s has implementation.",
                      SHOW(method_to_move));
    // Not adding conflicting methods
    DexMethod* existed_method = resolve_method(target_interface,
                                               method_to_move->get_name(),
                                               method_to_move->get_proto(),
                                               MethodSearch::Virtual);
    if (existed_method != nullptr) {
      always_assert_log(existed_method->get_code() == nullptr,
                        "Interface vmethod %s has implementation.",
                        SHOW(existed_method));
      TRACE(MEINT, 7, "Virtual method existed:\n");
      TRACE(MEINT, 7, SHOW(existed_method));
      TRACE(MEINT, 7, "\n");
      // Keep track of mapping of kept DexMethod and deleted DexMethod
      // so that we can also replace the deleted DexMethodRef in code.
      (*old_to_new_method)[method_to_move] = existed_method;
      continue;
    }
    TRACE(MEINT, 7, "Virtual method moved:\n");
    TRACE(MEINT, 7, SHOW(method_to_move));
    TRACE(MEINT, 7, "\n");
    DexMethodRef* methodref_in_context =
        DexMethod::get_method(target_intf_type,
                              method_to_move->get_name(),
                              method_to_move->get_proto());
    if (methodref_in_context != nullptr) {
      DexMethodRef::erase_method(methodref_in_context);
      // Even resolve_method returns nullptr, get_method might still return
      // some DexMethodRef, that means some where in code this DexMethodRef
      // is being used, also keep track of this so we can replace later.
      (*old_to_new_method)[methodref_in_context] = method_to_move;
    }
    from_interface->remove_method(method_to_move);
    DexMethodSpec spec;
    spec.cls = target_intf_type;
    method_to_move->change(spec,
                           false /* rename on collision */,
                           false /* update deobfuscated name */);
    target_interface->add_method(method_to_move);
  }
}

void move_fields_to_interface(DexClass* from_interface,
                              DexClass* target_interface) {
  DexType* target_intf_type = target_interface->get_type();
  auto sfields = from_interface->get_sfields();
  for (DexField* field : sfields) {
    TRACE(MEINT, 7, "Moving field ");
    TRACE(MEINT, 7, SHOW(field));
    TRACE(MEINT, 7, "\n");
    from_interface->remove_field(field);
    set_public(field);
    DexFieldSpec field_spec;
    field_spec.cls = target_intf_type;
    field->change(field_spec, true /* rename_on_collision */);
    target_interface->add_field(field);
  }
}

std::unordered_map<const DexType*, DexType*> merge_interfaces(
    const std::vector<DexClassSet>& to_merge,
    MergeInterfacePass::Metric* metric,
    std::unordered_map<DexMethodRef*, DexMethodRef*>* old_to_new_method) {
  std::unordered_map<const DexType*, DexType*> intf_merge_map;
  for (auto it = to_merge.begin(); it != to_merge.end(); ++it) {
    const DexClassSet& intf_set = *it;
    if (intf_set.size() <= 1) {
      // Might get candidate deleted before this method so left with one
      // or no interface candidate in group, don't proceed with those.
      continue;
    }
    // Record stats
    metric->interfaces_to_merge += intf_set.size();
    ++metric->interfaces_created;

    // Get arbitrary interface as merge target.
    auto set_start = intf_set.begin();
    DexClass* merge_to_intf = *set_start;
    TRACE(MEINT, 3, "merger:   %p\n    ", merge_to_intf->get_type());
    TRACE(MEINT, 3, SHOW(merge_to_intf));
    TRACE(MEINT, 3, "\n");

    // Get original interfaces of target interface and use that as the start
    // point of its new interfaces.
    std::unordered_set<DexType*> new_intfs;
    const auto& original_intf =
        merge_to_intf->get_interfaces()->get_type_list();
    new_intfs.insert(original_intf.begin(), original_intf.end());

    ++set_start;
    // Merge other interfaces into the interface we chose.
    for (auto set_it = set_start; set_it != intf_set.end(); ++set_it) {
      DexClass* interface_to_copy = (*set_it);
      TRACE(MEINT, 3, "merged:   %p\n    ", interface_to_copy->get_type());
      TRACE(MEINT, 3, SHOW(interface_to_copy));
      TRACE(MEINT, 3, "\n");
      intf_merge_map[interface_to_copy->get_type()] = merge_to_intf->get_type();
      // copy the methods
      move_methods_to_interface(interface_to_copy, merge_to_intf,
                                old_to_new_method);
      // copy the sfield
      move_fields_to_interface(interface_to_copy, merge_to_intf);
      // add the interfaces
      const auto& super_intfs =
          interface_to_copy->get_interfaces()->get_type_list();
      new_intfs.insert(super_intfs.begin(), super_intfs.end());
    }

    // Get rid of merge target in new interfaces set if it was added in.
    new_intfs.erase(merge_to_intf->get_type());
    // Set super interfaces to merged super interfaces.
    std::deque<DexType*> deque;
    for (const auto& intf : new_intfs) {
      deque.emplace_back(intf);
    }
    DexTypeList* implements = DexTypeList::make_type_list(std::move(deque));
    merge_to_intf->set_interfaces(implements);
  }
  return intf_merge_map;
}

void update_reference_for_code(
    const Scope& scope,
    const std::unordered_map<const DexType*, DexType*>& intf_merge_map,
    const std::unordered_map<DexMethodRef*, DexMethodRef*>& old_to_new_method) {
  auto patcher = [&old_to_new_method, &intf_merge_map](DexMethod*,
                                                       IRCode& code) {
    auto ii = InstructionIterable(code);
    for (auto it = ii.begin(); it != ii.end(); ++it) {
      IRInstruction* insn = it->insn;
      // Change method refs of merged interface method to corresponding method
      // in target interface.
      if (insn->has_method()) {
        DexMethodRef* meth_ref = insn->get_method();
        if (meth_ref == nullptr) {
          continue;
        }
        auto find_existed = old_to_new_method.find(meth_ref);
        if (find_existed != old_to_new_method.end()) {
          DexMethodRef* new_method_ref = find_existed->second;
          insn->set_method(new_method_ref);
        } else {
          // Handle method ref calling method in super interfaces.
          // For example if we have a mergeable interface A, it's super
          // interface B has a method do_something(), the code could
          // invoke this method through A.do_something(). When merge interface
          // A into another interface (let's say C), we need to change this
          // MethodRef to C.do_something().
          // TODO(suree404): if C has a super interface D that also have
          // function named do_something, rename one of the do_something if
          // they have code, ignore for abstract (no code) cases.
          auto find_method_class = intf_merge_map.find(meth_ref->get_class());
          if (find_method_class == intf_merge_map.end()) {
            continue;
          }
          DexType* target_type = find_method_class->second;
          DexMethodRef* methodref_in_context = DexMethod::get_method(
              target_type, meth_ref->get_name(), meth_ref->get_proto());
          if (methodref_in_context != nullptr) {
            insn->set_method(methodref_in_context);
          } else {
            DexMethodSpec spec;
            spec.cls = target_type;
            meth_ref->change(spec,
                             false /* rename on collision */,
                             false /* update deobfuscated name */);
          }
        }
        continue;
      }
      // Update simple type refs
      if (!insn->has_type() || insn->get_type() == nullptr) {
        continue;
      }
      DexType* ref_type = insn->get_type();
      const DexType* type = get_array_type_or_self(ref_type);
      if (intf_merge_map.count(type) == 0) {
        continue;
      }
      always_assert(type_class(type));
      DexType* merger_type = intf_merge_map.at(type);
      if (is_array(ref_type)) {
        insn->set_type(make_array_type(merger_type));
      } else {
        insn->set_type(merger_type);
      }
    }
  };
  walk::parallel::code(scope, patcher);
}

void remove_implements(
    const Scope& scope,
    const std::unordered_map<const DexType*, DexType*>& intf_merge_map) {
  // TODO(suree404): possible speed optimization, use type system to get
  // implementors and interface children and only update those.
  for (const auto& cls : scope) {
    bool got_one = false;
    for (const auto& cls_intf : cls->get_interfaces()->get_type_list()) {
      if (intf_merge_map.find(cls_intf) != intf_merge_map.end()) {
        got_one = true;
        break;
      }
    }
    if (!got_one) {
      continue;
    }
    TRACE(MEINT, 9, "Updating interface for %p\n", cls->get_type());
    std::unordered_set<DexType*> new_intfs;
    TRACE(MEINT, 9, "Original was:\n");
    for (DexType* cls_intf : cls->get_interfaces()->get_type_list()) {
      TRACE(MEINT, 9, "%p ", cls_intf);
      const auto& find_intf = intf_merge_map.find(cls_intf);
      if (find_intf != intf_merge_map.end()) {
        // This interface is merged interface, add its merger interface instead
        if (find_intf->second != cls->get_type()) {
          new_intfs.emplace(find_intf->second);
        }
      } else {
        // Not merged interface, safe to add.
        new_intfs.emplace(cls_intf);
      }
    }
    std::deque<DexType*> deque;
    TRACE(MEINT, 9, "\nAfter is:\n");
    for (DexType* intf : new_intfs) {
      TRACE(MEINT, 9, "%p ", intf);
      deque.emplace_back(intf);
    }
    TRACE(MEINT, 9, "\n");
    DexTypeList* implements = DexTypeList::make_type_list(std::move(deque));
    cls->set_interfaces(implements);
  }
}

void update_after_merge(
    const Scope& scope,
    const std::unordered_map<const DexType*, DexType*>& intf_merge_map,
    const std::unordered_map<DexMethodRef*, DexMethodRef*>& old_to_new_method) {
  type_reference::update_method_signature_type_references(scope,
                                                          intf_merge_map);
  type_reference::update_field_type_references(scope, intf_merge_map);
  update_reference_for_code(scope, intf_merge_map, old_to_new_method);
  remove_implements(scope, intf_merge_map);
}

// TODO(suree404): Remove this part and rely on RMU for cleaning up.
void remove_merged_interfaces(
    Scope& scope,
    const std::unordered_map<const DexType*, DexType*>& intf_merge_map) {
  if (intf_merge_map.empty()) return;
  Scope tscope(scope);
  scope.clear();
  for (DexClass* cls : tscope) {
    if (intf_merge_map.find(cls->get_type()) != intf_merge_map.end()) {
      TRACE(MEINT, 3, "Removing interface %s\n", SHOW(cls));
    } else {
      scope.push_back(cls);
    }
  }
}

void write_interface_merging_mapping_file(
    const std::unordered_map<const DexType*, DexType*>& intf_merge_map,
    const std::string& mapping_file) {
  if (mapping_file.empty()) {
    TRACE(MEINT, 1, "Interface merging mapping file not provided\n");
    return;
  }
  FILE* fd = fopen(mapping_file.c_str(), "w");
  std::stringstream out;
  for (const auto& pair : intf_merge_map) {
    out << SHOW(pair.first) << " -> " << SHOW(pair.second) << "\n";
  }
  fprintf(fd, "%s", out.str().c_str());
  fclose(fd);
  TRACE(MEINT, 1, "Writting interface merging mapping file finished\n");
}

} // namespace

void MergeInterfacePass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& conf,
                                  PassManager& mgr) {
  // Merging interfaces that are in seperate stores, or merging interfaces that
  // some are in primary dex and some are in secondary dexes will cause
  // trouble, so group classes by their stores and primary/secondary dexes
  // if they are in root store. Then pick interfaces that can merge together
  // in each group.
  std::vector<std::vector<DexClass*>> classes_groups;
  for (auto& dex_store : stores) {
    if (dex_store.is_root_store()) {
      auto& dexes = dex_store.get_dexen();
      auto& primary_dex = dexes[0];
      std::vector<DexClass*> primary_dex_classes;
      for (auto& cls : primary_dex) {
        primary_dex_classes.emplace_back(cls);
      }
      classes_groups.emplace_back(primary_dex_classes);
      std::vector<DexClass*> secondary_dex_classes;
      for (size_t index = 1; index < dexes.size(); ++index) {
        for (auto& cls : dexes[index]) {
          secondary_dex_classes.emplace_back(cls);
        }
      }
      classes_groups.emplace_back(secondary_dex_classes);
    } else {
      std::vector<DexClass*> non_root_store_classes;
      for (auto& dex : dex_store.get_dexen()) {
        for (auto& cls : dex) {
          non_root_store_classes.emplace_back(cls);
        }
      }
      classes_groups.emplace_back(non_root_store_classes);
    }
  }

  auto scope = build_class_scope(stores);

  auto can_merge = collect_can_merge(scope, classes_groups, &m_metric);
  // Remove interfaces that if merged could cause virtual method collision
  strip_out_collision(scope, &can_merge);
  strip_out_dmethod_relo_problem_intf(scope, &can_merge);
  std::unordered_map<DexMethodRef*, DexMethodRef*> old_to_new_method;
  auto intf_merge_map =
      merge_interfaces(can_merge, &m_metric, &old_to_new_method);
  update_after_merge(scope, intf_merge_map, old_to_new_method);
  remove_merged_interfaces(scope, intf_merge_map);
  post_dexen_changes(scope, stores);
  write_interface_merging_mapping_file(
      intf_merge_map, conf.metafile(m_merged_interface_mapping_file));

  mgr.set_metric("num_mergeable_interfaces", m_metric.interfaces_to_merge);
  mgr.set_metric("num_created_interfaces", m_metric.interfaces_created);
  mgr.set_metric("num_interfaces_in_anno_not_merging",
                 m_metric.interfaces_in_annotation);
}

static MergeInterfacePass s_pass;
