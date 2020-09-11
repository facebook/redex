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
#include "IROpcode.h"
#include "Resolver.h"
#include "Trace.h"
#include "TypeReference.h"
#include "Walkers.h"

namespace {

/**
 * If DontMergeState is STRICT, then don't merge no matter this
 * type is merger or mergeable.
 * If DontMergeState is CONDITIONAL, then don't merge if this
 * type is mergeable.
 */
enum DontMergeState { CONDITIONAL, STRICT };

void record_dont_merge_state(
    const DexType* type,
    DontMergeState state,
    std::unordered_map<const DexType*, DontMergeState>* dont_merge_status) {
  auto element_type = type::get_element_type_if_array(type);
  if (state == STRICT) {
    (*dont_merge_status)[element_type] = state;
    return;
  }
  const auto& find = dont_merge_status->find(element_type);
  if (find == dont_merge_status->end() || find->second != STRICT) {
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
    std::unordered_map<DexClass*, DexClass*>* mergeable_to_merger) {
  const auto& find_parent = dont_merge_status.find(parent_cls->get_type());
  const auto& find_child = dont_merge_status.find(child_cls->get_type());
  const auto& find_end = dont_merge_status.end();
  if (find_parent == find_end &&
      (find_child == find_end || find_child->second != STRICT)) {
    // Parent class is not referenced, and child class is not having STRICT
    // don't merge status, so we can merge parent class into child class.
    (*mergeable_to_merger)[parent_cls] = child_cls;
  } else if (find_child == find_end) {
    // Parent class is in don't remove set but child class is not. Check if we
    // can merge child class into parent class instead (which requires parent
    // class is not having STRICT don't merge status, and child class is
    // removable).
    if (find_parent->second != STRICT && can_delete(child_cls) &&
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
 * Gather IRinstructions in method that invoke-super methods in mergeable
 * class. Or IRcode that contains call to mergeable class's ctors.
 */
void get_call_to_super(
    DexMethod* method,
    DexClass* mergeable,
    std::unordered_map<DexMethod*, std::unordered_set<IRInstruction*>>*
        callee_to_insns,
    std::unordered_map<DexMethod*, std::unordered_set<IRCode*>>* init_callers) {
  if (!method->get_code()) {
    return;
  }
  for (auto& mie : InstructionIterable(method->get_code())) {
    auto insn = mie.insn;
    if (insn->has_method() &&
        insn->get_method()->get_class() == mergeable->get_type()) {
      DexMethodRef* insn_method = insn->get_method();
      DexMethod* insn_method_def =
          resolve_method(insn_method, MethodSearch::Any);
      if (insn_method_def &&
          insn_method_def->get_class() == mergeable->get_type()) {
        if (method::is_init(insn_method_def)) {
          (*init_callers)[insn_method_def].emplace(method->get_code());
          TRACE(VMERGE,
                5,
                "Changing init call %s:\n %s",
                SHOW(insn),
                SHOW(insn_method_def->get_code()));
        } else {
          (*callee_to_insns)[insn_method_def].emplace(insn);
          TRACE(VMERGE,
                5,
                "Replacing super call %s:\n %s",
                SHOW(insn),
                SHOW(insn_method_def->get_code()));
        }
      }
    }
  }
}

/**
 * Relocate callee methods in IRInstruction from mergeable class to merger
 * class. Modify IRInstruction accordingly.
 */
void handle_invoke_super(
    const std::unordered_map<DexMethod*, std::unordered_set<IRInstruction*>>&
        callee_to_insns,
    DexClass* merger,
    DexClass* mergeable) {
  for (const auto& callee_to_insn : callee_to_insns) {
    DexMethod* callee = callee_to_insn.first;
    mergeable->remove_method(callee);
    DexMethodSpec spec;
    spec.cls = merger->get_type();
    callee->change(spec,
                   true /* rename_on_collision */);
    merger->add_method(callee);
    for (auto insn : callee_to_insn.second) {
      if (insn->opcode() == OPCODE_INVOKE_SUPER) {
        insn->set_opcode(OPCODE_INVOKE_VIRTUAL);
      }
      insn->set_method(callee);
    }
  }
}

/**
 * Relocate ctor methods called in IRCode from mergeable class to merger
 * class. Add dummy parameter to avoid method collision, Add 'const 0's before
 * call to ctors and modify IRinstruction accordingly.
 */
void handle_invoke_init(
    const std::unordered_map<DexMethod*, std::unordered_set<IRCode*>>&
        init_callers,
    DexClass* merger,
    DexClass* mergeable) {
  for (const auto& callee_to_insn : init_callers) {
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
        DexMethod* insn_method_def =
            resolve_method(insn_method, MethodSearch::Direct);
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
    callee->change(spec,
                   false /* rename_on_collision */);
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
 *   5. Both classes are not in STRICT don't merge state, and mergeable is not
 *      in any don't merge state.
 *   6. Classes are not throwable.
 */
void collect_can_merge(
    const Scope& scope,
    const XStoreRefs& xstores,
    const std::unordered_map<const DexType*, DontMergeState>& dont_merge_status,
    std::unordered_map<DexClass*, DexClass*>* mergeable_to_merger) {
  ClassHierarchy ch = build_type_hierarchy(scope);
  auto throwables = get_all_children(ch, type::java_lang_Throwable());
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
      if (get_children(ch, child_type).size() != 0) {
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
        check_dont_merge_list(dont_merge_status, child_cls, cls,
                              mergeable_to_merger);
      }
    }
  }
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
      record_dont_merge_state(type, STRICT, dont_merge_status);
    }
  });
}

void record_code_reference(
    const Scope& scope,
    std::unordered_map<const DexType*, DontMergeState>* dont_merge_status,
    std::unordered_set<DexMethod*>* referenced_methods) {
  walk::opcodes(
      scope,
      [](DexMethod* method) { return true; },
      [&](DexMethod* method, IRInstruction* insn) {
        std::unordered_set<DexType*> types_to_check;
        if (insn->has_type()) {
          types_to_check.emplace(insn->get_type());
          if (is_instance_of(insn->opcode())) {
            // We don't want to merge class if either merger or
            // mergeable was ever accessed in instance_of to prevent
            // semantic error.
            record_dont_merge_state(insn->get_type(), STRICT,
                                    dont_merge_status);
            return;
          }
        } else if (insn->has_field()) {
          DexField* field = resolve_field(insn->get_field());
          if (field != nullptr) {
            if (field->get_class() != insn->get_field()->get_class() ||
                !can_rename(field)) {
              // If a field reference need to be resolved, don't merge as
              // renaming it might cause problems.
              // If a field that can't be renamed is being referenced. Don't
              // merge it as we need the field and this field can't be renamed
              // if having collision.
              // TODO(suree404): can improve.
              record_dont_merge_state(field->get_class(), CONDITIONAL,
                                      dont_merge_status);
            }
          } else {
            record_dont_merge_state(insn->get_field()->get_class(), CONDITIONAL,
                                    dont_merge_status);
          }
        } else if (insn->has_method()) {
          DexMethod* callee =
              resolve_method(insn->get_method(), MethodSearch::Any);
          if (callee != nullptr) {
            if (type_class(method->get_class())->get_super_class() ==
                    callee->get_class() &&
                (insn->opcode() == OPCODE_INVOKE_SUPER ||
                 insn->opcode() == OPCODE_INVOKE_DIRECT)) {
              // If this method call is invoke-super or invoke-direct
              // from child class method to parent class method, then this
              // should be fine as we can relocate the methods.
              return;
            }
            types_to_check.emplace(callee->get_class());
            // Don't merge an abstract class if its method is invoked through
            // invoke-virtual, it means we might need to keep both method
            // in parent and child class, and need to face true-virtual
            // renaming issue.
            // TODO(suree404): oportunity to improve this.
            if (insn->opcode() == OPCODE_INVOKE_VIRTUAL &&
                callee->get_class() == insn->get_method()->get_class()) {
              DexClass* callee_class = type_class(callee->get_class());
              if (callee_class && is_abstract(callee_class)) {
                record_dont_merge_state(callee->get_class(), CONDITIONAL,
                                        dont_merge_status);
              }
            }
            if ((insn->opcode() == OPCODE_INVOKE_STATIC ||
                 insn->opcode() == OPCODE_INVOKE_DIRECT) &&
                type_class(method->get_class())->get_super_class() !=
                    callee->get_class()) {
              // Record abstract class's static and direct methods that are
              // referenced somewhere other than their child class. This means
              // we need to keep those methods.
              DexClass* callee_class = type_class(callee->get_class());
              if (callee_class && is_abstract(callee_class)) {
                referenced_methods->emplace(callee);
              }
            }
          }
          types_to_check.emplace(insn->get_method()->get_class());
        }

        for (auto type_to_check : types_to_check) {
          const DexType* self_type =
              type::get_element_type_if_array(type_to_check);
          DexClass* cls = type_class(self_type);
          if (cls && !is_abstract(cls)) {
            // If a type is referenced and not a abstract type then
            // add it to don't use this type as mergeable.
            record_dont_merge_state(self_type, CONDITIONAL, dont_merge_status);
          }
        }
      });
}

// Record a type as don't merge as a mergeable if
//  1. It's in a native method's signature.
//  2. It's in signature of a method of itself. TODO(suree404): this case was
//     added because I was looking at implementation of singleimpl and it
//     have this case. I don't really understand why this case exist in
//     in singleimpl, so exclude it for now and probably can optimize later.
//  3. It's in method signature and it is not an abstract class.
void record_method_signature(
    const Scope& scope,
    std::unordered_map<const DexType*, DontMergeState>* dont_merge_status) {
  auto check_method_sig = [&](const DexType* type, DexMethod* method) {
    bool method_is_native = is_native(method);
    DexType* self_type = method->get_class();
    if (method_is_native || type == self_type) {
      record_dont_merge_state(type, CONDITIONAL, dont_merge_status);
    } else {
      DexClass* cls = type_class(type);
      if (cls && (!is_abstract(cls) || !can_rename(method))) {
        // If a type is referenced and not a abstract type then add it to
        // don't use this type as mergeable.
        record_dont_merge_state(type, CONDITIONAL, dont_merge_status);
      }
    }
  };
  walk::methods(scope, [&](DexMethod* method) {
    DexProto* proto = method->get_proto();
    const DexType* rtype = type::get_element_type_if_array(proto->get_rtype());
    check_method_sig(rtype, method);
    DexTypeList* args = proto->get_args();
    for (const DexType* it : args->get_type_list()) {
      const DexType* extracted_type = type::get_element_type_if_array(it);
      check_method_sig(extracted_type, method);
    }
  });
}

void record_black_list(
    const Scope& scope,
    std::unordered_map<const DexType*, DontMergeState>* dont_merge_status,
    const std::vector<std::string>& blacklist) {
  if (blacklist.size() == 0) {
    return;
  }
  walk::classes(scope, [&](DexClass* cls) {
    // Mark class in blacklist as STRICT don't merge
    for (const auto& name : blacklist) {
      if (strstr(cls->get_name()->c_str(), name.c_str()) != nullptr) {
        TRACE(VMERGE,
              5,
              "%s | %s | %u",
              SHOW(cls),
              cls->rstate.str().c_str(),
              can_delete(cls));
        record_dont_merge_state(cls->get_type(), STRICT, dont_merge_status);
        return;
      }
    }
  });
}

/**
 * Remove pair of classes from merging if they both have clinit function.
 */
void remove_both_have_clinit(
    std::unordered_map<DexClass*, DexClass*>* mergeable_to_merger) {
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
      record_dont_merge_state(field->get_type(), CONDITIONAL,
                              dont_merge_status);
    }
  });
}

void record_referenced(
    const Scope& scope,
    std::unordered_map<const DexType*, DontMergeState>* dont_merge_status,
    const std::vector<std::string>& blacklist,
    std::unordered_set<DexMethod*>* referenced_methods) {
  record_annotation(scope, dont_merge_status);
  record_code_reference(scope, dont_merge_status, referenced_methods);
  record_field_reference(scope, dont_merge_status);
  record_method_signature(scope, dont_merge_status);
  record_black_list(scope, dont_merge_status, blacklist);
}

void move_fields(DexClass* from_cls, DexClass* to_cls) {
  DexType* target_cls_type = to_cls->get_type();
  auto move_field = [&](DexField* field) {
    TRACE(VMERGE, 5, "move field : %s ", SHOW(field));
    from_cls->remove_field(field);
    DexFieldSpec field_spec;
    field_spec.cls = target_cls_type;
    field->change(field_spec, true /* rename_on_collision */);

    TRACE(VMERGE, 5, "field after : %s ", SHOW(field));
    to_cls->add_field(field);
  };
  auto sfields = from_cls->get_sfields();
  auto ifields = from_cls->get_ifields();
  for (DexField* field : sfields) {
    move_field(field);
  }
  for (DexField* field : ifields) {
    move_field(field);
  }
}

void update_references(const Scope& scope,
                       const std::unordered_map<DexType*, DexType*>& update_map,
                       const std::unordered_map<DexMethodRef*, DexMethodRef*>&
                           methodref_update_map) {
  walk::opcodes(
      scope,
      [](DexMethod* method) { return true; },
      [&](DexMethod* method, IRInstruction* insn) {
        if (update_map.count(method->get_class())) {
          // Ignore references in methods in classes that are going to be
          // removed.
          return;
        }
        if (insn->has_type()) {
          auto ref_type = insn->get_type();
          DexType* type =
              const_cast<DexType*>(type::get_element_type_if_array(ref_type));
          if (update_map.count(type) == 0) {
            return;
          }
          auto merger_type = update_map.at(type);
          if (type::is_array(ref_type)) {
            auto array_merger_type = type::make_array_type(merger_type);
            insn->set_type(array_merger_type);
          } else {
            insn->set_type(const_cast<DexType*>(merger_type));
          }
        } else if (insn->has_field()) {
          DexField* field = resolve_field(insn->get_field());
          if (field != nullptr) {
            always_assert_log(
                update_map.count(field->get_class()) == 0,
                "Vertical Merging: Field reference still exist\n");
          }
          always_assert_log(update_map.count(insn->get_field()->get_class()) ==
                                0,
                            "Vertical Merging: Field reference still exist\n");
        } else if (insn->has_method()) {
          DexMethodRef* insn_method = insn->get_method();
          DexMethod* callee = resolve_method(insn_method, MethodSearch::Any);
          if (callee != nullptr) {
            auto find = methodref_update_map.find(callee);
            if (find != methodref_update_map.end()) {
              insn->set_method(find->second);
              return;
            }
            always_assert_log(
                update_map.count(callee->get_class()) == 0,
                "Vertical Merging: Method reference still exist: %s\nin %s\n",
                SHOW(insn), SHOW(method));
          }
          auto find = methodref_update_map.find(insn_method);
          if (find != methodref_update_map.end()) {
            insn->set_method(find->second);
          } else {
            auto find_mergeable = update_map.find(insn_method->get_class());
            if (find_mergeable != update_map.end()) {
              always_assert_log(
                  type_class(find_mergeable->second)->get_super_class() ==
                      type_class(insn_method->get_class())->get_super_class(),
                  "Vertical Merging: subclass ref exist %s\nin %s\n",
                  SHOW(insn),
                  SHOW(method));
              if (insn->opcode() == OPCODE_INVOKE_SUPER) {
                insn->set_method(DexMethod::make_method(
                    type_class(insn_method->get_class())->get_super_class(),
                    insn_method->get_name(),
                    insn_method->get_proto()));
              } else {
                insn->set_method(
                    DexMethod::make_method(find_mergeable->second,
                                           insn_method->get_name(),
                                           insn_method->get_proto()));
              }
            }
          }
        }
      });
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

void remove_merged(
    Scope& scope,
    const std::unordered_map<DexClass*, DexClass*>& mergeable_to_merger) {
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

} // namespace

/**
 * For an invoke-super or invoke-direct call on mergeable's method,
 * we move the method to merger class and change invoke call.
 */
void VerticalMergingPass::change_super_calls(
    const std::unordered_map<DexClass*, DexClass*>& mergeable_to_merger) {
  auto process_subclass_methods = [&](DexClass* merger, DexClass* mergeable) {
    std::unordered_map<DexMethod*, std::unordered_set<IRInstruction*>>
        callee_to_insns;
    std::unordered_map<DexMethod*, std::unordered_set<IRCode*>> init_callers;
    for (DexMethod* method : merger->get_dmethods()) {
      get_call_to_super(method, mergeable, &callee_to_insns, &init_callers);
    }
    for (DexMethod* method : merger->get_vmethods()) {
      get_call_to_super(method, mergeable, &callee_to_insns, &init_callers);
    }
    handle_invoke_super(callee_to_insns, merger, mergeable);
    handle_invoke_init(init_callers, merger, mergeable);
  };

  for (const auto& pair : mergeable_to_merger) {
    DexClass* merger = pair.second;
    DexClass* mergeable = pair.first;
    if (merger->get_super_class() == mergeable->get_type()) {
      process_subclass_methods(merger, mergeable);
    }
  }
}

void VerticalMergingPass::move_methods(
    DexClass* from_cls,
    DexClass* to_cls,
    bool is_merging_super_to_sub,
    const std::unordered_set<DexMethod*>& referenced_methods,
    std::unordered_map<DexMethodRef*, DexMethodRef*>* methodref_update_map) {
  DexType* target_cls_type = to_cls->get_type();
  auto move_method = [&](DexMethod* method) {
    TRACE(VMERGE, 5, "%s | %s | %s", SHOW(from_cls), SHOW(to_cls),
          SHOW(method));
    if (method::is_clinit(method)) {
      // We have removed pairs that both have clinit, so we can just move
      // clinit to target class.
      auto methodref_in_context = DexMethod::get_method(
          target_cls_type, method->get_name(), method->get_proto());
      if (methodref_in_context) {
        (*methodref_update_map)[methodref_in_context] = method;
        DexMethodRef::erase_method(methodref_in_context);
      }
      from_cls->remove_method(method);
      DexMethodSpec spec;
      spec.cls = target_cls_type;
      method->change(spec, false /* rename_on_collision */);
      to_cls->add_method(method);
      return;
    }
    if (is_merging_super_to_sub) {
      // Super class is being merged into subclass
      auto methodref_in_context = DexMethod::get_method(
          target_cls_type, method->get_name(), method->get_proto());
      if (methodref_in_context) {
        TRACE(VMERGE,
              5,
              "ALREADY EXISTED METHODREF %s",
              SHOW(methodref_in_context));
        DexMethod* method_def = resolve_method(
            to_cls, method->get_name(), method->get_proto(), MethodSearch::Any);
        always_assert_log(
            method_def != nullptr,
            "Found a method ref can't be resolve during merging.\n");
        TRACE(VMERGE, 5, "RESOLVED to %s", SHOW(method_def));
        if (method_def->get_class() != target_cls_type) {
          // the method resolved is not defined in target class, so the method
          // in mergeable class should have implementation for the method ref
          // in target class. Remove the method ref in target class and
          // substitute it with real method implementation.
          (*methodref_update_map)[methodref_in_context] = method;
          DexMethodRef::erase_method(methodref_in_context);
          TRACE(VMERGE, 5, "Erasing method ref.");
        } else {
          if (referenced_methods.count(method)) {
            // Static or direct method. Safe to move
            always_assert(can_rename(method));
            from_cls->remove_method(method);
            DexMethodSpec spec;
            spec.cls = target_cls_type;
            method->change(spec,
                           true /* rename_on_collision */);
            to_cls->add_method(method);
          } else {
            // Otherwise we shouldn't care for the method as the method in
            // mergeable should not be referenced.
            // But we need to combine annotation of method and their reference
            // state into merger class's method because we are basically merging
            // two methods.
            method_def->combine_annotations_with(method);
            method_def->rstate.join_with(method->rstate);
            if (method->get_code() == nullptr) {
              // A method ref on abstract method that was implemented in
              // subclass. Record and substitute it with method ref of its
              // subclass.
              (*methodref_update_map)[method] = methodref_in_context;
            }
          }
          return;
        }
      }
      // If it got here, it is either there is no conflict when moving method
      // to target class, or the conflicting method ref in target class is
      // removed.
      from_cls->remove_method(method);
      DexMethodSpec spec;
      spec.cls = target_cls_type;
      method->change(spec,
                     false /* rename_on_collision */);
      to_cls->add_method(method);
    } else {
      // Subclass is being merged into super class. Just discard methods as
      // they are not being referenced, otherwise they won't be mergeable.
      return;
    }
  };
  auto dmethod = from_cls->get_dmethods();
  auto vmethod = from_cls->get_vmethods();
  for (DexMethod* method : dmethod) {
    TRACE(VMERGE, 5, "dmethods:");
    move_method(method);
  }
  for (DexMethod* method : vmethod) {
    TRACE(VMERGE, 5, "vmethods:");
    move_method(method);
  }
}

void VerticalMergingPass::merge_classes(
    const Scope& scope,
    const std::unordered_map<DexClass*, DexClass*>& mergeable_to_merger,
    const std::unordered_set<DexMethod*>& referenced_methods) {
  std::unordered_map<DexType*, DexType*> update_map;
  std::unordered_map<DexMethodRef*, DexMethodRef*> methodref_update_map;
  for (const auto& pair : mergeable_to_merger) {
    DexClass* merger = pair.second;
    DexClass* mergeable = pair.first;
    bool is_merging_super_to_sub =
        merger->get_super_class() == mergeable->get_type();
    move_fields(mergeable, merger);
    move_methods(mergeable,
                 merger,
                 is_merging_super_to_sub,
                 referenced_methods,
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
  type_reference::TypeRefUpdater updater(update_map);
  updater.update_methods_fields(scope);
}

void VerticalMergingPass::run_pass(DexStoresVector& stores,
                                   ConfigFiles& conf,
                                   PassManager& mgr) {
  auto scope = build_class_scope(stores);

  std::unordered_map<const DexType*, DontMergeState> dont_merge_status;
  std::unordered_set<DexMethod*> referenced_methods;
  record_referenced(scope, &dont_merge_status, m_blacklist,
                    &referenced_methods);
  XStoreRefs xstores(stores);
  std::unordered_map<DexClass*, DexClass*> mergeable_to_merger;
  collect_can_merge(scope, xstores, dont_merge_status, &mergeable_to_merger);
  remove_both_have_clinit(&mergeable_to_merger);

  change_super_calls(mergeable_to_merger);

  merge_classes(scope, mergeable_to_merger, referenced_methods);
  remove_merged(scope, mergeable_to_merger);
  post_dexen_changes(scope, stores);
  mgr.set_metric("num_pair_to_merge", mergeable_to_merger.size());
}

static VerticalMergingPass s_pass;
