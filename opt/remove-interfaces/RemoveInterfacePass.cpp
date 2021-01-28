/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveInterfacePass.h"

#include "Creators.h"
#include "DexStoreUtil.h"
#include "DexUtil.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Show.h"
#include "SwitchDispatch.h"
#include "Trace.h"
#include "TypeReference.h"
#include "TypeSystem.h"
#include "Walkers.h"

using namespace type_reference;
using MethodOrderedSet = std::set<DexMethod*, dexmethods_comparator>;

namespace {

std::vector<Location> get_args_for(DexProto* proto, MethodCreator* mc) {
  std::vector<Location> args;
  size_t args_size = proto->get_args()->size();
  for (size_t arg_loc = 0; arg_loc < args_size; ++arg_loc) {
    args.push_back(mc->get_local(arg_loc));
  }

  return args;
}

DexAnnotationSet* get_anno_set(DexType* anno_type) {
  auto anno = new DexAnnotation(anno_type, DexAnnotationVisibility::DAV_BUILD);
  DexAnnotationSet* anno_set = new DexAnnotationSet();
  anno_set->add_annotation(anno);
  return anno_set;
}
DexMethod* materialized_dispatch(DexType* owner, MethodCreator* mc) {
  auto dispatch = mc->create();
  TRACE(RM_INTF,
        9,
        "Generated dispatch %s\n%s",
        SHOW(dispatch),
        SHOW(dispatch->get_code()));
  type_class(owner)->add_method(dispatch);
  return dispatch;
}

/**
 * Generate an interface call dispatch.
 * Here is an example with two targets:
 * If we have interface Fragment extending RootInterface,
 * two classes, FirstModel and SecondModel implementing Fragment.
 * Each one of them implements method int getA().
 * For an interface call to Fragment.getA(), here is the dispatch we generate.
 *
 * $dispatch$getA:(LRootInterface;)I
 * 0000: instance-of v0, v1, LFirstModel;
 * 0002: if-eqz v0, 000b
 * 0004: check-cast v1, LFirstModel;
 * 0006: invoke-virtual {v1}, LFirstModel;.getA:()I
 * 0009: move-result v0
 * 000a: return v0
 * 000b: check-cast v1, LSecondModel;
 * 000d: invoke-virtual {v1}, LSecondModel;.getA:()I
 * 0010: move-result v0
 * 0011: return v0
 */
DexMethod* generate_dispatch(const DexType* base_type,
                             const std::vector<DexMethod*>& targets,
                             const DexMethod* intf_method,
                             const bool keep_debug_info,
                             DexType* dispatch_anno) {
  DexType* dispatch_owner = targets.front()->get_class();
  // Owner and proto
  auto orig_name = std::string(intf_method->c_str());
  auto front_meth = targets.front();
  auto new_arg_list = prepend_and_make(front_meth->get_proto()->get_args(),
                                       const_cast<DexType*>(base_type));
  auto rtype = front_meth->get_proto()->get_rtype();
  auto new_proto = DexProto::make_proto(rtype, new_arg_list);
  auto dispatch_name =
      dispatch::gen_dispatch_name(dispatch_owner, new_proto, orig_name);

  TRACE(RM_INTF, 9, "generating dispatch %s.%s for targets of size %d",
        SHOW(dispatch_owner), dispatch_name->c_str(), targets.size());
  auto anno_set = get_anno_set(dispatch_anno);
  auto mc =
      new MethodCreator(dispatch_owner, dispatch_name, new_proto,
                        ACC_STATIC | ACC_PUBLIC, anno_set, keep_debug_info);
  // Variable setup
  auto self_loc = mc->get_local(0);
  auto type_test_loc = mc->make_local(type::_boolean());
  auto ret_loc = new_proto->is_void() ? mc->get_local(0) // not used
                                      : mc->make_local(new_proto->get_rtype());
  std::vector<Location> args = get_args_for(new_proto, mc);
  auto mb = mc->get_main_block();

  /**
   * In case all interface scopes can only be resolved to a single concrete
   * implementation, we generate a simple single call dispatch and expect the
   * inliner to deal with it.
   * TODO(zwei): We can revisit this later, since this case only applies to
   * TreeModels. Perhaps there's an even better way to handle this.
   */
  if (targets.size() == 1) {
    auto target_meth = targets.front();
    auto target_type = target_meth->get_class();
    mb->check_cast(self_loc, target_type);
    mb->invoke(OPCODE_INVOKE_VIRTUAL, target_meth, args);
    if (!new_proto->is_void()) {
      mb->move_result(ret_loc, new_proto->get_rtype());
    }
    mb->ret(new_proto->get_rtype(), ret_loc);
    return materialized_dispatch(dispatch_owner, mc);
  }
  // Construct dispatchs
  for (size_t idx = 0; idx < targets.size(); idx++) {
    auto target_meth = targets.at(idx);
    auto target_type = target_meth->get_class();
    MethodBlock* curr_block;

    if (idx < targets.size() - 1) {
      mb->instance_of(self_loc, type_test_loc,
                      const_cast<DexType*>(target_type));
      curr_block = mb->if_testz(OPCODE_IF_EQZ, type_test_loc);
    } else {
      // Last case
      curr_block = mb;
    }

    curr_block->check_cast(self_loc, target_type);
    curr_block->invoke(OPCODE_INVOKE_VIRTUAL, target_meth, args);
    if (!new_proto->is_void()) {
      curr_block->move_result(ret_loc, new_proto->get_rtype());
    }
    curr_block->ret(new_proto->get_rtype(), ret_loc);
  }
  // Finalizing
  return materialized_dispatch(dispatch_owner, mc);
}

void update_interface_calls(
    const Scope& scope,
    const std::unordered_map<DexMethod*, DexMethod*>& old_to_new_callee) {
  auto patcher = [&old_to_new_callee](DexMethod* meth, IRInstruction* insn) {
    if (!insn->has_method()) {
      return;
    }
    const auto method =
        resolve_method(insn->get_method(), opcode_to_search(insn), meth);
    if (method == nullptr || old_to_new_callee.count(method) == 0) {
      return;
    }
    auto new_callee = old_to_new_callee.at(method);
    TRACE(RM_INTF, 9, "Updated call %s to %s", SHOW(insn), SHOW(new_callee));
    insn->set_method(new_callee);
    insn->set_opcode(OPCODE_INVOKE_STATIC);
  };
  walk::parallel::opcodes(scope, patcher);
}

/**
 * Build the new interface list for type impl with intf_to_remove removed from
 * its interface list. We do so by merging the remainder of impl's interface
 * list and the super types of intf_to_remove.
 */
DexTypeList* get_new_impl_list(const DexType* impl,
                               const DexType* intf_to_remove) {
  std::set<DexType*, dextypes_comparator> new_intfs;
  auto cls = type_class(impl);
  for (const auto intf : cls->get_interfaces()->get_type_list()) {
    if (intf == intf_to_remove) {
      continue;
    }
    new_intfs.insert(intf);
  }
  auto cls_to_remove = type_class(intf_to_remove);
  auto& super_intfs = cls_to_remove->get_interfaces()->get_type_list();
  new_intfs.insert(super_intfs.begin(), super_intfs.end());
  std::deque<DexType*> deque(new_intfs.begin(), new_intfs.end());
  return DexTypeList::make_type_list(std::move(deque));
}

const DexType* get_replacement_type(const TypeSystem& type_system,
                                    const DexType* to_remove,
                                    const DexType* root) {
  TypeSet parent_intfs;
  type_system.get_all_super_interfaces(to_remove, parent_intfs);
  if (parent_intfs.count(root) > 0) {
    return root;
  }
  TRACE(RM_INTF, 9, "Replacing %s with java.lang.Object;", SHOW(to_remove));
  return type::java_lang_Object();
}

/**
 * Currently we exclude types referenced in certain operations that we do not
 * properly handle yet.
 *
 * NEW_INSTANCE: it shouldn't happen on interface types in valid bytecode. Just
 * exlude this case for safety.
 *
 * INSTANCE_OF: uncommon. Correct type checking at runtime
 * is not trivial and comes with cost.
 *
 * CONST_CLASS: also not very common. We don't quite understand
 * the implication of the subsequent reflections.
 *
 * TODO(zwei): update type reference in an arbitrarily nested array
 * type. NEW_ARRAY & FILLED_NEW_ARRAY: are not excluded anymore. But we need to
 * add the machinery to update type reference in an arbitrarily nested array
 * type.
 */
bool is_opcode_excluded(const IROpcode op) {
  return op == OPCODE_NEW_INSTANCE || op == OPCODE_CONST_CLASS ||
         op == OPCODE_INSTANCE_OF;
}

void remove_interface_references(
    const Scope& scope,
    const TypeSystem& type_system,
    const DexType* root,
    const std::unordered_set<const DexType*>& interfaces) {

  auto patcher = [&](DexMethod*, IRInstruction* insn) {
    if (!insn->has_type()) {
      return;
    }
    const auto ref_type = insn->get_type();
    auto type = type::get_element_type_if_array(ref_type);
    if (interfaces.count(type) == 0) {
      return;
    }
    auto opcode = insn->opcode();
    always_assert_log(!is_opcode_excluded(opcode),
                      "Unexpected opcode %s on %s\n", SHOW(opcode), SHOW(type));
    always_assert(type_class(type));
    auto new_type = get_replacement_type(type_system, type, root);
    if (type::is_array(ref_type)) {
      const auto array_merger_type = type::make_array_type(new_type);
      insn->set_type(array_merger_type);
      TRACE(RM_INTF,
            9,
            " removing %s referencing array type of %s",
            SHOW(insn),
            SHOW(type));
    } else {
      insn->set_type(const_cast<DexType*>(new_type));
      TRACE(RM_INTF, 9, " removing %s referencing %s", SHOW(insn), SHOW(type));
    }
  };

  walk::parallel::opcodes(scope, patcher);

  std::unordered_map<const DexType*, DexType*> old_to_new;
  for (const auto intf : interfaces) {
    auto new_type = get_replacement_type(type_system, intf, root);
    old_to_new[intf] = const_cast<DexType*>(new_type);
  }
  auto& parent_to_children =
      type_system.get_class_scopes().get_parent_to_children();
  update_method_signature_type_references(scope, old_to_new,
                                          parent_to_children);
  update_field_type_references(scope, old_to_new);
}

size_t exclude_unremovables(const Scope& scope,
                            const DexStoresVector& stores,
                            const TypeSystem& type_system,
                            bool include_primary_dex,
                            TypeSet& candidates) {
  size_t count = 0;
  always_assert(stores.size());
  XStoreRefs xstores(stores);

  // Skip intfs with single or none implementor. For some reason, they are
  // not properly removed by either SingleImpl or UnreferencedInterfacesPass.
  // They are not the focus of this pass. We should address them elsewhere.
  std::vector<const DexType*> intf_list(candidates.begin(), candidates.end());
  for (auto intf : intf_list) {
    const auto& impls = type_system.get_implementors(intf);
    if (impls.size() <= 1) {
      TRACE(RM_INTF, 5, "Excluding %s with impls of size %d", SHOW(intf),
            impls.size());
      candidates.erase(intf);
      count++;
      continue;
    }

    const auto& non_root_store_types =
        get_non_root_store_types(stores, xstores, impls, include_primary_dex);
    if (!non_root_store_types.empty()) {
      TRACE(RM_INTF, 5, "Excluding %s with non root store implementors",
            SHOW(intf));
      candidates.erase(intf);
      count++;
      continue;
    }
  }

  // Scan unsupported opcodes.
  auto patcher = [&](DexMethod* meth) {
    std::unordered_set<const DexType*> current_excluded;
    auto code = meth->get_code();
    if (!code) {
      return current_excluded;
    }
    for (const auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      if (!insn->has_type() || insn->get_type() == nullptr) {
        continue;
      }
      auto type = type::get_element_type_if_array(insn->get_type());
      if (candidates.count(type) == 0) {
        continue;
      }
      if (is_opcode_excluded(insn->opcode())) {
        TRACE(RM_INTF, 5, "Excluding %s %s in %s", SHOW(insn->opcode()),
              SHOW(type), SHOW(meth));
        current_excluded.insert(type);
      }
    }

    return current_excluded;
  };

  auto excluded_by_opcode = walk::parallel::methods<
      std::unordered_set<const DexType*>,
      MergeContainers<std::unordered_set<const DexType*>>>(scope, patcher);

  for (const auto type : excluded_by_opcode) {
    candidates.erase(type);
  }
  count += excluded_by_opcode.size();
  return count;
}

/**
 * For a given implementor type, find a virtual method sharing the same virtual
 * scope. The found virtual method is an interface dispatch target for the
 * implementor type. We try to find a public concrete method in the top ancestor
 * if possible. In this way we could minimize the outgoing method refernce in
 * the generated dispatch. Therefore, having smaller or more common dispatches.
 */
DexMethod* find_matching_virtual_method(const TypeSystem& type_system,
                                        const DexType* owner,
                                        const VirtualScope* scope) {
  for (const auto& vmeth : scope->methods) {
    auto method = vmeth.first;
    if (!method->is_def() || !is_public(method)) {
      continue;
    }
    if (type_system.is_subtype(method->get_class(), owner)) {
      TRACE(RM_INTF, 9, "Matched target %s for %s", SHOW(method), SHOW(owner));
      return method;
    }
  }

  return nullptr;
}

/**
 * Find all possible dispatch targets for each implementor type with in the
 * given interface scope. We scan all the overlapping virtual scope to enumerate
 * the targets for each implementor type. Some implementors belonging to the
 * same virtual scope maybe share the same common target.
 */
MethodOrderedSet find_dispatch_targets(const TypeSystem& type_system,
                                       const InterfaceScope& intf_scope,
                                       TypeSet implementors) {
  MethodOrderedSet targets;
  for (const auto& virt_scope : intf_scope) {
    auto top_def = virt_scope->methods.front();
    TRACE(RM_INTF, 5, "Scanning virt scope %s[%ld]", SHOW(top_def.first),
          virt_scope->methods.size());
    std::vector<const DexType*> matched;
    for (const auto impl : implementors) {
      if (type_system.is_subtype(virt_scope->type, impl)) {
        auto target =
            find_matching_virtual_method(type_system, impl, virt_scope);
        always_assert(target != nullptr);
        targets.insert(target);
        matched.push_back(impl);
      }
    }
    for (const auto impl : matched) {
      implementors.erase(impl);
    }
  }

  // All implementor types should've been matched by now.
  always_assert(implementors.empty());
  return targets;
}

/**
 * Include interfaces extended by the children of root. These parent
 * interfaces are not a child of the root. But since they are part of the
 * interface inheritance hierarchy, we want to remove them as well.
 */
void include_parent_interfaces(const DexType* root, TypeSet& interfaces) {
  TypeSet parent_interfaces;
  for (const auto intf : interfaces) {
    auto parent_intfs = type_class(intf)->get_interfaces()->get_type_list();
    for (const auto parent_intf : parent_intfs) {
      if (parent_intf != root) {
        parent_interfaces.insert(parent_intf);
      }
    }
  }
  size_t size_before = interfaces.size();
  interfaces.insert(parent_interfaces.begin(), parent_interfaces.end());
  TRACE(RM_INTF, 5, "Found parent interfaces %d",
        interfaces.size() - size_before);
}

} // namespace

bool RemoveInterfacePass::is_leaf(const TypeSystem& type_system,
                                  const DexType* intf) {
  auto intf_children = type_system.get_interface_children(intf);
  size_t num_removed = 0;
  for (const auto child : intf_children) {
    if (m_removed_interfaces.count(child) > 0) {
      ++num_removed;
    }
  }
  return num_removed == intf_children.size();
}

void RemoveInterfacePass::remove_inheritance(const Scope& scope,
                                             const TypeSystem& type_system,
                                             const TypeSet& interfaces) {
  for (const auto intf : interfaces) {
    always_assert(is_leaf(type_system, intf));
    auto impls = type_system.get_implementors(intf);
    for (const auto impl : impls) {
      TRACE(RM_INTF, 5, "Remove inheritance for %s on %s", SHOW(intf),
            SHOW(impl));
      auto new_impl_list = get_new_impl_list(impl, intf);
      type_class(impl)->set_interfaces(new_impl_list);
    }
    type_class(intf)->set_interfaces(DexTypeList::make_type_list({}));
  }
}

TypeSet RemoveInterfacePass::remove_leaf_interfaces(
    const Scope& scope,
    const DexType* root,
    const TypeSet& interfaces,
    const TypeSystem& type_system) {
  TypeSet leaf_interfaces;
  for (const auto intf : interfaces) {
    if (is_leaf(type_system, intf)) {
      leaf_interfaces.insert(intf);
    }
  }

  std::unordered_map<DexMethod*, DexMethod*> intf_meth_to_dispatch;
  for (const auto intf : leaf_interfaces) {
    TRACE(RM_INTF, 5, "Found leaf interface %s", SHOW(intf));
    const auto& implementors = type_system.get_implementors(intf);
    auto intf_methods = type_class(intf)->get_vmethods();
    for (const auto meth : intf_methods) {
      TRACE(RM_INTF, 5, "Finding virt scope for %s", SHOW(meth));
      auto intf_scope = type_system.find_interface_scope(meth);
      MethodOrderedSet found_targets =
          find_dispatch_targets(type_system, intf_scope, implementors);
      std::vector<DexMethod*> dispatch_targets(found_targets.begin(),
                                               found_targets.end());
      auto replacement_type = get_replacement_type(type_system, intf, root);
      auto dispatch =
          generate_dispatch(replacement_type, dispatch_targets, meth,
                            m_keep_debug_info, m_interface_dispatch_anno);
      m_dispatch_stats[dispatch_targets.size()]++;
      intf_meth_to_dispatch[meth] = dispatch;
    }
  }
  update_interface_calls(scope, intf_meth_to_dispatch);
  remove_inheritance(scope, type_system, leaf_interfaces);
  m_num_interface_removed += leaf_interfaces.size();
  return leaf_interfaces;
}

void RemoveInterfacePass::remove_interfaces_for_root(
    const Scope& scope,
    const DexStoresVector& stores,
    const DexType* root,
    const TypeSystem& type_system) {
  TRACE(RM_INTF, 5, "Processing root %s", SHOW(root));
  TypeSet interfaces;
  type_system.get_all_interface_children(root, interfaces);
  include_parent_interfaces(root, interfaces);

  m_total_num_interface += interfaces.size();
  m_num_interface_excluded += exclude_unremovables(
      scope, stores, type_system, m_include_primary_dex, interfaces);

  TRACE(RM_INTF, 5, "removable interfaces %ld", interfaces.size());
  TypeSet removed =
      remove_leaf_interfaces(scope, root, interfaces, type_system);

  while (!removed.empty()) {
    for (const auto intf : removed) {
      interfaces.erase(intf);
      m_removed_interfaces.insert(intf);
    }
    TRACE(RM_INTF, 5, "non-leaf removable interfaces %ld", interfaces.size());
    removed = remove_leaf_interfaces(scope, root, interfaces, type_system);
  }

  // Update type reference to removed interfaces all at once.
  remove_interface_references(scope, type_system, root, m_removed_interfaces);

  if (traceEnabled(RM_INTF, 9)) {
    TypeSystem updated_ts(scope);
    for (const auto intf : interfaces) {
      TRACE(RM_INTF, 9, "unremoved interface %s", SHOW(intf));
      TypeSet children;
      updated_ts.get_all_interface_children(intf, children);
      for (const auto cintf : children) {
        TRACE(RM_INTF, 9, "  child %s", SHOW(cintf));
      }
    }
  }
}

void RemoveInterfacePass::bind_config() {
  bind("interface_roots", {}, m_interface_roots, Configurable::default_doc(),
       Configurable::bindflags::types::warn_if_unresolvable);
  bind("include_primary_dex", false, m_include_primary_dex);
  bind("keep_debug_info", false, m_keep_debug_info);
  bind_required("interface_dispatch_anno", m_interface_dispatch_anno,
                Configurable::default_doc(),
                Configurable::bindflags::types::error_if_unresolvable);
}

void RemoveInterfacePass::run_pass(DexStoresVector& stores,
                                   ConfigFiles&,
                                   PassManager& mgr) {
  auto scope = build_class_scope(stores);
  TypeSystem type_system(scope);
  for (const auto root : m_interface_roots) {
    remove_interfaces_for_root(scope, stores, root, type_system);
  }
  mgr.incr_metric("num_total_interface", m_total_num_interface);
  mgr.incr_metric("num_interface_excluded", m_num_interface_excluded);
  mgr.incr_metric("num_interface_removed", m_num_interface_removed);
  TRACE(RM_INTF, 5, "total number of interfaces %ld", m_total_num_interface);
  TRACE(RM_INTF, 5, "number of excluded interfaces %ld",
        m_num_interface_excluded);
  TRACE(RM_INTF, 5, "number of removed interfaces %ld",
        m_num_interface_removed);

  for (const auto& stat : m_dispatch_stats) {
    std::stringstream metric;
    metric << "num_dispatch_" << stat.first;
    mgr.incr_metric(metric.str(), stat.second);
  }
}

static RemoveInterfacePass s_pass;
