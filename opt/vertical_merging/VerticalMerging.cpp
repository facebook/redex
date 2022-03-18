/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VerticalMerging.h"

#include "ClassHierarchy.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "EditableCfgAdapter.h"
#include "IROpcode.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Trace.h"
#include "TypeReference.h"
#include "Walkers.h"

namespace {
using ClassMap = std::unordered_map<DexClass*, DexClass*>;
using MethodRefMap = std::unordered_map<DexMethodRef*, DexMethodRef*>;

bool is_internal_def(DexMethodRef* method) {
  if (!method || !method->is_def()) {
    return false;
  }
  auto cls = type_class(method->get_class());
  if (!cls || cls->is_external()) {
    return false;
  }
  for (auto m : cls->get_vmethods()) {
    if (m == method) {
      return true;
    }
  }
  for (auto m : cls->get_dmethods()) {
    if (m == method) {
      return true;
    }
  }
  // Investigate the case if we hit it.
  not_reached_log(
      "%s is removed from its container class but its definition is not "
      "deleted.",
      SHOW(method));
}

/**
 * If DontMergeState is kStrict, then don't merge no matter this
 * type is merger or mergeable.
 * If DontMergeState is kConditional, then don't merge if this
 * type is mergeable.
 */
enum DontMergeState { kConditional, kStrict };

void record_dont_merge_state(
    const DexType* type,
    DontMergeState state,
    std::unordered_map<const DexType*, DontMergeState>* dont_merge_status) {
  auto element_type = type::get_element_type_if_array(type);
  if (state == kStrict) {
    (*dont_merge_status)[element_type] = state;
    return;
  }
  const auto& find = dont_merge_status->find(element_type);
  if (find == dont_merge_status->end() || find->second != kStrict) {
    (*dont_merge_status)[element_type] = state;
  }
}

/**
 * Check child class and parent class's DontMergeState and decide which class
 * should be merged into which class, or not merge at all.
 */
void check_dont_merge_list(
    const std::unordered_map<const DexType*, DontMergeState>& dont_merge_status,
    DexClass* child_cls,
    DexClass* parent_cls,
    ClassMap* mergeable_to_merger) {
  const auto& find_parent = dont_merge_status.find(parent_cls->get_type());
  const auto& find_child = dont_merge_status.find(child_cls->get_type());
  const auto& find_end = dont_merge_status.end();
  if (find_parent == find_end &&
      (find_child == find_end || find_child->second != kStrict)) {
    // Parent class is not referenced, and child class is not having kStrict
    // don't merge status, so we can merge parent class into child class.
    (*mergeable_to_merger)[parent_cls] = child_cls;
  } else if (find_child == find_end) {
    // Parent class is in don't remove set but child class is not. Check if we
    // can merge child class into parent class instead (which requires parent
    // class is not having kStrict don't merge status, and child class is
    // removable).
    if (find_parent->second != kStrict && can_delete(child_cls) &&
        can_rename(child_cls)) {
      if (is_abstract(child_cls)) {
        for (auto method : child_cls->get_vmethods()) {
          if (method->get_code()) {
            return;
          }
        }
        for (auto method : child_cls->get_dmethods()) {
          if (method->get_code()) {
            return;
          }
        }
      }
      (*mergeable_to_merger)[child_cls] = parent_cls;
    }
  }
}

/**
 * Gather IRinstructions in method that invoke-super methods in parent_mergeable
 * class or invoke-direct parent_mergeable's constructors.
 */
void get_call_to_super(
    DexMethod* method,
    DexClass* parent_mergeable,
    std::unordered_map<DexMethodRef*, std::vector<IRInstruction*>>*
        callee_to_insns,
    std::unordered_map<DexMethod*, std::vector<IRCode*>>* init_callers) {
  if (!method->get_code()) {
    return;
  }
  for (auto& mie : InstructionIterable(method->get_code())) {
    auto insn = mie.insn;
    if (!insn->has_method()) {
      continue;
    }
    auto insn_method = insn->get_method();
    if (insn_method->get_class() == parent_mergeable->get_type()) {
      if (method::is_init(insn_method)) {
        auto insn_method_def = insn_method->as_def();
        redex_assert(insn_method_def);
        (*init_callers)[insn_method_def].push_back(method->get_code());
        TRACE(VMERGE, 5, "Changing init call %s", SHOW(insn));
      } else if (opcode::is_invoke_super(insn->opcode())) {
        (*callee_to_insns)[insn_method].push_back(insn);
        TRACE(VMERGE, 5, "Replacing super call %s", SHOW(insn));
      }
    }
  }
}

using SuperCall = std::pair<DexMethodRef*, std::vector<IRInstruction*>>;
using InitCall = std::pair<DexMethod*, std::vector<IRCode*>>;

/**
 * Relocate callee methods in IRInstruction from mergeable class to merger
 * class. Modify IRInstruction accordingly.
 */
void handle_invoke_super(
    const std::unordered_map<DexMethodRef*, std::vector<IRInstruction*>>&
        callee_to_insns,
    DexClass* merger,
    DexClass* parent_mergeable) {
  std::vector<SuperCall> super_calls(callee_to_insns.begin(),
                                     callee_to_insns.end());
  std::sort(super_calls.begin(), super_calls.end(),
            [](const SuperCall& call1, SuperCall& call2) {
              return compare_dexmethods(call1.first, call2.first);
            });
  for (const auto& callee_to_insn : super_calls) {
    auto callee_ref = callee_to_insn.first;
    if (is_internal_def(callee_ref)) {
      //  invoke-super Parent.v => invoke-virtual Child.relocated_parent_v
      auto callee = callee_ref->as_def();
      parent_mergeable->remove_method(callee);
      DexMethodSpec spec;
      spec.cls = merger->get_type();
      callee->change(spec, true /* rename_on_collision */);
      merger->add_method(callee);
      for (auto insn : callee_to_insn.second) {
        redex_assert(insn->opcode() == OPCODE_INVOKE_SUPER);
        insn->set_opcode(OPCODE_INVOKE_VIRTUAL);
        insn->set_method(callee);
      }
    } else {
      // The only pure ref we need handle.
      // invoke-super Parent.v => invoke-super GrandParent.v
      auto new_ref = DexMethod::make_method(parent_mergeable->get_super_class(),
                                            callee_ref->get_name(),
                                            callee_ref->get_proto());
      for (auto insn : callee_to_insn.second) {
        insn->set_method(new_ref);
      }
    }
  }
}

/**
 * Relocate ctor methods called in IRCode from mergeable class to merger
 * class. Add dummy parameter to avoid method collision, Add 'const 0's before
 * call to ctors and modify IRinstruction accordingly.
 */
void handle_invoke_init(
    const std::unordered_map<DexMethod*, std::vector<IRCode*>>& init_callers,
    DexClass* merger,
    DexClass* mergeable) {
  std::vector<InitCall> initcalls(init_callers.begin(), init_callers.end());
  std::sort(initcalls.begin(), initcalls.end(),
            [](const InitCall& call1, const InitCall& call2) {
              return compare_dexmethods(call1.first, call2.first);
            });
  for (const auto& callee_to_insn : initcalls) {
    DexMethod* callee = callee_to_insn.first;
    size_t num_orig_args = callee->get_proto()->get_args()->size();
    DexProto* new_proto = merger->get_type()->get_non_overlapping_proto(
        callee->get_name(), callee->get_proto());
    size_t num_add_args = new_proto->get_args()->size() - num_orig_args;
    size_t num_orig_src = num_orig_args + 1;
    callee->add_load_params(num_add_args);
    for (auto code : callee_to_insn.second) {
      auto ii = InstructionIterable(code);
      auto end = ii.end();
      for (auto it = ii.begin(); it != end; ++it) {
        auto* insn = it->insn;
        if (insn->opcode() != OPCODE_INVOKE_DIRECT) {
          continue;
        }
        DexMethodRef* insn_method = insn->get_method();
        DexMethod* insn_method_def = insn_method->as_def();
        if (insn_method_def == callee) {
          size_t current_add = 0;
          insn->set_srcs_size(num_add_args + num_orig_src);
          while (current_add < num_add_args) {
            auto temp = code->allocate_temp();
            IRInstruction* new_insn = new IRInstruction(OPCODE_CONST);
            new_insn->set_literal(0);
            new_insn->set_dest(temp);
            code->insert_before(it.unwrap(), new_insn);
            insn->set_src(num_orig_src + current_add, temp);
            ++current_add;
          }
          insn->set_method(callee);
        }
      }
    }
    mergeable->remove_method(callee);
    DexMethodSpec spec;
    spec.cls = merger->get_type();
    spec.proto = new_proto;
    callee->change(spec, false /* rename_on_collision */);
    merger->add_method(callee);
  }
}

/**
 * Collect pairs of parent child classes that are mergeable with the following
 * constraints:
 *   1. Two classes need to be in same store (or primary or secondary dexes).
 *   2. Parent class has only one child class.
 *   3. Both classes are not external classes and not interface.
 *   4. Mergeable class is deletable.
 *   5. Both classes are not in kStrict don't merge state, and mergeable is not
 *      in any don't merge state.
 *   6. Classes are not throwable.
 */
ClassMap collect_can_merge(
    const Scope& scope,
    const XStoreRefs& xstores,
    const std::unordered_map<const DexType*, DontMergeState>& dont_merge_status,
    size_t* num_single_extend_pairs) {
  ClassMap mergeable_to_merger;
  ClassHierarchy ch = build_type_hierarchy(scope);
  auto throwables = get_all_children(ch, type::java_lang_Throwable());
  *num_single_extend_pairs = 0;
  for (DexClass* cls : scope) {
    if (cls && !cls->is_external() && !is_interface(cls) && can_delete(cls) &&
        can_rename(cls) && !throwables.count(cls->get_type())) {
      DexType* cls_type = cls->get_type();
      const auto& children_types = get_children(ch, cls_type);
      if (children_types.size() != 1) {
        continue;
      }
      const DexType* child_type = *children_types.begin();
      if (throwables.count(child_type)) {
        continue;
      }
      if (!get_children(ch, child_type).empty()) {
        // TODO(suree404): we are skipping pairs that child class still have
        // their subclasses, but we might still be able to optimize this case.
        continue;
      }
      if (xstores.get_store_idx(child_type) !=
          xstores.get_store_idx(cls_type)) {
        // Check if cls_type and child_type is in the same dex, if not then
        // skip and proceed.
        continue;
      }
      DexClass* child_cls = type_class_internal(child_type);
      if (child_cls) {
        (*num_single_extend_pairs)++;
        check_dont_merge_list(dont_merge_status, child_cls, cls,
                              &mergeable_to_merger);
      }
    }
  }
  return mergeable_to_merger;
}

void record_annotation(
    const Scope& scope,
    std::unordered_map<const DexType*, DontMergeState>* dont_merge_status) {
  // Remove class if it is the type of an annotation.
  // TODO(suree404): Merge the classes even though it appears in annotation?
  walk::annotations(scope, [&](DexAnnotation* anno) {
    std::vector<DexType*> types_in_anno;
    anno->gather_types(types_in_anno);
    for (const auto& type : types_in_anno) {
      record_dont_merge_state(type, kStrict, dont_merge_status);
    }
  });
}

/**
 * 1. Analyze type usages.
 * 2. To simplify the method/field references updating, exclude the pure refs.
 * When ResolveRefsPass runs before the pass, the step should not drop many
 * mergeables.
 */
void record_code_reference(
    const Scope& scope,
    std::unordered_map<const DexType*, DontMergeState>* dont_merge_status) {
  walk::opcodes(
      scope,
      [](DexMethod* method) { return true; },
      [&](DexMethod* method, IRInstruction* insn) {
        if (insn->has_type()) {
          auto type = type::get_element_type_if_array(insn->get_type());
          if (opcode::is_instance_of(insn->opcode())) {
            // We don't want to merge class if either merger or
            // mergeable was ever accessed in instance_of to prevent
            // semantic error.
            record_dont_merge_state(type, kStrict, dont_merge_status);
            return;
          } else {
            DexClass* cls = type_class(type);
            if (cls && !is_abstract(cls)) {
              // If a type is referenced and not an abstract type then
              // add it to don't use this type as mergeable.
              record_dont_merge_state(type, kConditional, dont_merge_status);
              TRACE(VMERGE, 9, "dont_merge %s as mergeable for type usage: %s",
                    SHOW(type), SHOW(insn));
            }
          }
        } else if (insn->has_field()) {
          DexField* field = resolve_field(insn->get_field());
          if (field != nullptr) {
            bool resolve_differently =
                field->get_class() != insn->get_field()->get_class();
            if (resolve_differently) {
              // If a field reference need to be resolved, don't merge as
              // renaming it might cause problems.
              // If a field that can't be renamed is being referenced. Don't
              // merge it as we need the field and this field can't be renamed
              // if having collision.
              // TODO(suree404): can improve.
              record_dont_merge_state(field->get_class(), kStrict,
                                      dont_merge_status);
              record_dont_merge_state(insn->get_field()->get_class(), kStrict,
                                      dont_merge_status);
            }
          } else {
            record_dont_merge_state(insn->get_field()->get_class(),
                                    kConditional, dont_merge_status);
          }
        } else if (insn->has_method()) {
          auto callee_ref = insn->get_method();
          if (opcode::is_invoke_super(insn->opcode())) {
            // The only allowed pure ref is in invoke-super.
            return;
          }
          if (!is_internal_def(callee_ref)) {
            record_dont_merge_state(callee_ref->get_class(), kStrict,
                                    dont_merge_status);
            TRACE(VMERGE, 9, "dont_merge %s for pure ref %s",
                  SHOW(callee_ref->get_class()), SHOW(callee_ref));
            DexMethod* callee = resolve_method(callee_ref, MethodSearch::Any);
            if (callee) {
              record_dont_merge_state(callee->get_class(), kStrict,
                                      dont_merge_status);
              TRACE(VMERGE, 9,
                    "dont_merge %s for it may be invoked as a pure ref %s",
                    SHOW(callee->get_class()), SHOW(callee_ref));
            }
          }
        }
      });
}

/**
 * When a method is native or not renamable, we can not change its signature.
 * Record a type as don't merge as a mergeable if it's used in a such method's
 * signature.
 */
void record_method_signature(
    const Scope& scope,
    std::unordered_map<const DexType*, DontMergeState>* dont_merge_status) {
  walk::methods(scope, [&](DexMethod* method) {
    if (is_native(method) || !can_rename(method)) {
      DexProto* proto = method->get_proto();
      record_dont_merge_state(proto->get_rtype(), kConditional,
                              dont_merge_status);
      DexTypeList* args = proto->get_args();
      for (const DexType* type : args->get_type_list()) {
        record_dont_merge_state(type, kConditional, dont_merge_status);
      }
    }
  });
}

void record_blocklist(
    const Scope& scope,
    std::unordered_map<const DexType*, DontMergeState>* dont_merge_status,
    const std::vector<std::string>& blocklist) {
  if (blocklist.empty()) {
    return;
  }
  walk::classes(scope, [&](DexClass* cls) {
    // Mark class in blocklist as kStrict don't merge
    for (const auto& name : blocklist) {
      if (strstr(cls->get_name()->c_str(), name.c_str()) != nullptr) {
        TRACE(VMERGE,
              5,
              "%s | %s | %u",
              SHOW(cls),
              cls->rstate.str().c_str(),
              can_delete(cls));
        record_dont_merge_state(cls->get_type(), kStrict, dont_merge_status);
        return;
      }
    }
  });
}

/**
 * Remove pair of classes from merging if they both have clinit function.
 */
void remove_both_have_clinit(ClassMap* mergeable_to_merger) {
  std::vector<DexClass*> to_delete;
  for (const auto& pair : *mergeable_to_merger) {
    if (pair.first->get_clinit() != nullptr &&
        pair.second->get_clinit() != nullptr) {
      to_delete.emplace_back(pair.first);
    }
  }
  for (DexClass* cls : to_delete) {
    mergeable_to_merger->erase(cls);
  }
}

/**
 * Don't merge a class if it is a field's type and this field can't be
 * renamed.
 */
void record_field_reference(
    const Scope& scope,
    std::unordered_map<const DexType*, DontMergeState>* dont_merge_status) {
  walk::fields(scope, [&](DexField* field) {
    if (!can_rename(field)) {
      record_dont_merge_state(field->get_type(), kConditional,
                              dont_merge_status);
    }
  });
}

void record_referenced(
    const Scope& scope,
    std::unordered_map<const DexType*, DontMergeState>* dont_merge_status,
    const std::vector<std::string>& blocklist) {
  record_annotation(scope, dont_merge_status);
  record_code_reference(scope, dont_merge_status);
  record_field_reference(scope, dont_merge_status);
  record_method_signature(scope, dont_merge_status);
  record_blocklist(scope, dont_merge_status, blocklist);
}

void move_fields(DexClass* from_cls, DexClass* to_cls) {
  DexType* target_cls_type = to_cls->get_type();
  auto fields = from_cls->get_all_fields();
  for (DexField* field : fields) {
    TRACE(VMERGE, 5, "move field : %s ", SHOW(field));
    from_cls->remove_field(field);
    DexFieldSpec field_spec;
    field_spec.cls = target_cls_type;
    field->change(field_spec, true /* rename_on_collision */);

    TRACE(VMERGE, 5, "field after : %s ", SHOW(field));
    to_cls->add_field(field);
  }
}

void update_references(const Scope& scope,
                       const std::unordered_map<DexType*, DexType*>& update_map,
                       const MethodRefMap& methodref_update_map) {
  walk::parallel::opcodes(
      scope,
      [&update_map](DexMethod* method) {
        // Ignore references in methods in classes that are going to be
        // removed.
        return !update_map.count(method->get_class());
      },
      [&](DexMethod* method, IRInstruction* insn) {
        if (insn->has_type()) {
          auto ref_type = insn->get_type();
          DexType* type =
              const_cast<DexType*>(type::get_element_type_if_array(ref_type));
          auto find_mergeable = update_map.find(type);
          if (find_mergeable == update_map.end()) {
            return;
          }
          always_assert_log(insn->opcode() != OPCODE_NEW_INSTANCE,
                            "Vertical Merging: type reference still exists %s",
                            SHOW(insn));
          auto merger_type = find_mergeable->second;
          if (type::is_array(ref_type)) {
            auto array_merger_type = type::make_array_type(merger_type);
            insn->set_type(array_merger_type);
          } else {
            insn->set_type(const_cast<DexType*>(merger_type));
          }
        } else if (insn->has_field()) {
          auto insn_field = insn->get_field();
          always_assert_log(!update_map.count(insn_field->get_class()),
                            "Vertical Merging: Field reference still exists %s",
                            SHOW(insn));
        } else if (insn->has_method()) {
          auto insn_method = insn->get_method();
          auto find = methodref_update_map.find(insn_method);
          if (find != methodref_update_map.end()) {
            insn->set_method(find->second);
          } else {
            always_assert_log(
                !update_map.count(insn_method->get_class()),
                "Vertical Merging: Method reference still exists %s in %s",
                SHOW(insn), SHOW(method));
          }
        }
      });
  // Update type refs in all field or method specs.
  type_reference::TypeRefUpdater updater(update_map);
  updater.update_methods_fields(scope);
}

void update_implements(DexClass* from_cls, DexClass* to_cls) {
  std::set<DexType*, dextypes_comparator> new_intfs;
  TRACE(VMERGE, 5, "interface before : ");
  for (const auto& cls_intf : to_cls->get_interfaces()->get_type_list()) {
    TRACE(VMERGE, 5, "  %s", SHOW(cls_intf));
    new_intfs.emplace(cls_intf);
  }
  for (const auto& cls_intf : from_cls->get_interfaces()->get_type_list()) {
    new_intfs.emplace(cls_intf);
  }
  std::deque<DexType*> deque;

  TRACE(VMERGE, 5, "interface after : ");
  for (const auto& intf : new_intfs) {
    TRACE(VMERGE, 5, "  %s", SHOW(intf));
    deque.emplace_back(intf);
  }

  auto implements = DexTypeList::make_type_list(std::move(deque));
  to_cls->set_interfaces(implements);
}

void remove_merged(Scope& scope, const ClassMap& mergeable_to_merger) {
  if (mergeable_to_merger.empty()) {
    return;
  }
  for (const auto& pair : mergeable_to_merger) {
    TRACE(VMERGE,
          5,
          "Removing class | %s | merged into | %s",
          SHOW(pair.first),
          SHOW(pair.second));
  }

  scope.erase(remove_if(scope.begin(), scope.end(),
                        [&](DexClass* cls) {
                          return mergeable_to_merger.find(cls) !=
                                 mergeable_to_merger.end();
                        }),
              scope.end());
}

/**
 * Try to resolve the virtual calls on a mergeable type to be a method ref on
 * its merger. If fail, we remove the mergeable-merger pair.
 */
void resolve_virtual_calls_to_merger(const Scope& scope,
                                     ClassMap& mergeable_to_merger) {
  ConcurrentSet<DexClass*> excluded_mergeables;
  ConcurrentMap<IRInstruction*, DexMethodRef*> resolved_virtual_calls;
  walk::parallel::code(scope, [&](DexMethod* /* method */, IRCode& code) {
    editable_cfg_adapter::iterate(&code, [&](MethodItemEntry& mie) {
      auto insn = mie.insn;
      if (opcode::is_invoke_virtual(insn->opcode())) {
        auto mergeable_method_ref = insn->get_method();
        auto container = type_class(mergeable_method_ref->get_class());
        if (!container) {
          return editable_cfg_adapter::LOOP_CONTINUE;
        }
        auto find_merger = mergeable_to_merger.find(container);
        if (find_merger != mergeable_to_merger.end() &&
            !excluded_mergeables.count(container)) {
          auto merger_method_ref = DexMethod::get_method(
              find_merger->second->get_type(), mergeable_method_ref->get_name(),
              mergeable_method_ref->get_proto());
          // Merger is the subclass.
          if (find_merger->second->get_super_class() == container->get_type()) {
            // XXX(fengliu): The possible overriding from subclasses of the
            // merger class is not checked because the case is excluded earlier
            // in collect_can_merge.
            if (merger_method_ref && is_internal_def(merger_method_ref)) {
              resolved_virtual_calls.insert({insn, merger_method_ref});
            }
          } else { // Merger is the superclass.
            if (resolve_virtual(find_merger->second,
                                mergeable_method_ref->get_name(),
                                mergeable_method_ref->get_proto())) {
              merger_method_ref =
                  DexMethod::make_method(find_merger->second->get_type(),
                                         mergeable_method_ref->get_name(),
                                         mergeable_method_ref->get_proto());
              resolved_virtual_calls.insert({insn, merger_method_ref});
            } else {
              // There is no instance of the mergeable class. So virtual calls
              // on the mergeable class should be invalid or unreachable. To
              // handle the "impossible" case, we can remove the virtual call or
              // simply not do the merging. Here we exclude the mergeable for
              // simplicity.
              excluded_mergeables.insert(container);
              TRACE(VMERGE, 5,
                    "Exclude a pair: virtual call %s is not resolvable to the "
                    "superclass %s",
                    SHOW(insn), SHOW(find_merger->second));
            }
          }
        }
      }
      return editable_cfg_adapter::LOOP_CONTINUE;
    });
  });
  for (auto cls : excluded_mergeables) {
    mergeable_to_merger.erase(cls);
  }
  for (auto& pair : resolved_virtual_calls) {
    auto insn = pair.first;
    auto container = type_class(insn->get_method()->get_class());
    if (mergeable_to_merger.count(container)) {
      insn->set_method(pair.second);
    }
  }
}

} // namespace

/**
 * 1. For an invoke-direct call on parent's constructor, move the method to
 * merger class and resolve conflicts.
 *
 * 2. For an invoke-super call on a parent's method
 *
 * a) When Parent.v is a pure ref, we update it to a method ref on grandparent
 * class.
 *
 *  invoke-super Parent.v => invoke-super GrandParent.v
 *
 * b) When Parent.v is a method definition, we relocate it to the child class
 * and rename it if conflict. These invocations can only be the merger class.
 *
 *  invoke-super Parent.v => invoke-virtual Child.relocated_parent_v
 *
 * At the time, bellow invocation can be different when child overrides the
 * method. These invocatoins should be resolved to child ref before running into
 * this.
 *
 *  invoke-virtual Parent.v => invoke-virtual Child.v
 *
 */
void VerticalMergingPass::change_super_calls(
    const ClassMap& mergeable_to_merger) {

  // Update invoke-super and invoke-direct constructor.
  // The invoke-super Parent.v could only be called from child class.
  // The invoke-direct Parent.<init> could be called only from child or from
  // parent's constructors.
  auto process_subclass_methods = [&](DexClass* child, DexClass* parent) {
    std::unordered_map<DexMethodRef*, std::vector<IRInstruction*>>
        callee_to_insns;
    std::unordered_map<DexMethod*, std::vector<IRCode*>> init_callers;
    for (DexMethod* method : child->get_dmethods()) {
      get_call_to_super(method, parent, &callee_to_insns, &init_callers);
    }
    for (DexMethod* method : child->get_vmethods()) {
      get_call_to_super(method, parent, &callee_to_insns, &init_callers);
    }
    for (DexMethod* method : parent->get_dmethods()) {
      get_call_to_super(method, parent, &callee_to_insns, &init_callers);
    }
    handle_invoke_super(callee_to_insns, child, parent);
    handle_invoke_init(init_callers, child, parent);
  };

  for (const auto& pair : mergeable_to_merger) {
    DexClass* merger = pair.second;
    DexClass* mergeable = pair.first;
    if (merger->get_super_class() == mergeable->get_type()) {
      process_subclass_methods(merger, mergeable);
    }
  }
}

void VerticalMergingPass::move_methods(DexClass* from_cls,
                                       DexClass* to_cls,
                                       bool is_merging_super_to_sub,
                                       MethodRefMap* methodref_update_map) {
  DexType* target_cls_type = to_cls->get_type();
  TRACE(VMERGE, 5, "Move methods from %s to %s:", SHOW(from_cls), SHOW(to_cls));
  auto move_method = [&](DexMethod* method, bool rename_on_collision) {
    from_cls->remove_method(method);
    DexMethodSpec spec;
    spec.cls = target_cls_type;
    method->change(spec, rename_on_collision);
    to_cls->add_method(method);
  };
  auto all_methods = from_cls->get_all_methods();
  for (DexMethod* method : all_methods) {
    TRACE(VMERGE, 5, "%s | %s | %s", SHOW(from_cls), SHOW(to_cls),
          SHOW(method));
    if (method::is_clinit(method)) {
      // We have removed pairs that both have clinit, so we can just move
      // clinit to target class.
      auto target_method_ref = DexMethod::get_method(
          target_cls_type, method->get_name(), method->get_proto());
      if (target_method_ref) {
        DexMethodRef::erase_method(target_method_ref);
      }
      move_method(method, false /* rename_on_collision */);
    } else if (is_merging_super_to_sub) {
      // Super class is being merged into subclass
      auto target_method_ref = DexMethod::get_method(
          target_cls_type, method->get_name(), method->get_proto());
      if (target_method_ref) {
        TRACE(VMERGE, 5, "ALREADY EXISTED METHODREF %s",
              SHOW(target_method_ref));
        if (!is_internal_def(target_method_ref)) {
          // the method resolved is not defined in target class, so the method
          // in mergeable class should have implementation for the method ref
          // in target class. Remove the method ref in target class and
          // substitute it with real method implementation.
          (*methodref_update_map)[target_method_ref] = method;
          DexMethodRef::erase_method(target_method_ref);
          TRACE(VMERGE, 5, "Erasing method ref.");
          move_method(method, false /* rename_on_collision */);
        } else {
          if (is_constructor(method)) {
            // Referenced constructors are already handled in
            // change_super_calls. The rest constructors are unused and they are
            // discarded.
            continue;
          } else if (!method->is_virtual()) {
            // Static or direct method. Safe to move
            always_assert(can_rename(method));
            move_method(method, true /* rename_on_collision */);
          } else {
            // Otherwise the method is virtual and child class overrides the
            // method in parent, we shouldn't care for the method as it is dead
            // code. But we need to combine annotation of method and their
            // reference state into merger class's method because we are
            // basically merging two methods.
            auto target_method_def = target_method_ref->as_def();
            target_method_def->combine_annotations_with(method);
            target_method_def->rstate.join_with(method->rstate);
            (*methodref_update_map)[method] = target_method_ref;
          }
        }
      } else {
        move_method(method, false /* rename_on_collision */);
      }
    } else {
      // Subclass is being merged into super class. Just discard the instance
      // methods as they should not be referenced, otherwise they won't be
      // mergeable. Move the non-constructor static methods from subclass to
      // super class.
      if (is_static(method) && !is_constructor(method)) {
        move_method(method, true /* rename_on_collision */);
      }
    }
  }
}

void VerticalMergingPass::merge_classes(const Scope& scope,
                                        const ClassMap& mergeable_to_merger) {
  std::unordered_map<DexType*, DexType*> update_map;
  // To store the needed changes from `Mergeable.method` to `Merger.method`.
  MethodRefMap methodref_update_map;

  change_super_calls(mergeable_to_merger);

  for (const auto& pair : mergeable_to_merger) {
    DexClass* merger = pair.second;
    DexClass* mergeable = pair.first;
    bool is_merging_super_to_sub =
        merger->get_super_class() == mergeable->get_type();
    move_fields(mergeable, merger);
    move_methods(mergeable, merger, is_merging_super_to_sub,
                 &methodref_update_map);
    if (is_merging_super_to_sub) {
      // We are merging super class into sub class, set merger's super class to
      // mergeable's super class.
      merger->set_super_class(mergeable->get_super_class());
      update_implements(mergeable, merger);
    }
    update_map[mergeable->get_type()] = merger->get_type();
    // Combine mergeable classes annotation and reference state with those
    // of merger classes.
    merger->combine_annotations_with(mergeable);
    merger->rstate.join_with(mergeable->rstate);
  }
  update_references(scope, update_map, methodref_update_map);
}

void VerticalMergingPass::run_pass(DexStoresVector& stores,
                                   ConfigFiles& conf,
                                   PassManager& mgr) {
  auto scope = build_class_scope(stores);

  std::unordered_map<const DexType*, DontMergeState> dont_merge_status;
  record_referenced(scope, &dont_merge_status, m_blocklist);
  XStoreRefs xstores(stores);
  size_t num_single_extend;
  auto mergeable_to_merger =
      collect_can_merge(scope, xstores, dont_merge_status, &num_single_extend);

  remove_both_have_clinit(&mergeable_to_merger);
  resolve_virtual_calls_to_merger(scope, mergeable_to_merger);

  merge_classes(scope, mergeable_to_merger);
  remove_merged(scope, mergeable_to_merger);
  post_dexen_changes(scope, stores);
  mgr.set_metric("num_single_extend", num_single_extend);
  mgr.set_metric("num_merged", mergeable_to_merger.size());
}

static VerticalMergingPass s_pass;
