/**
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
#include "TypeSystem.h"
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
  if (state == STRICT) {
    (*dont_merge_status)[type] = state;
    return;
  }
  const auto& find = dont_merge_status->find(type);
  if (find == dont_merge_status->end() || find->second != STRICT) {
    (*dont_merge_status)[type] = state;
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
    if (find_parent->second != STRICT && can_delete(child_cls)) {
      (*mergeable_to_merger)[child_cls] = parent_cls;
    }
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
 */
void collect_can_merge(
    const Scope& scope,
    const XStoreRefs& xstores,
    const std::unordered_map<const DexType*, DontMergeState>& dont_merge_status,
    std::unordered_map<DexClass*, DexClass*>* mergeable_to_merger) {
  TypeSystem ts(scope);
  for (DexClass* cls : scope) {
    if (cls && !cls->is_external() && !is_interface(cls) && can_delete(cls)) {
      DexType* cls_type = cls->get_type();
      const auto& children_types = ts.get_children(cls->get_type());
      if (children_types.size() != 1) {
        continue;
      }
      const DexType* child_type = *children_types.begin();
      if (ts.get_children(child_type).size() != 0) {
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
        check_dont_merge_list(
            dont_merge_status, child_cls, cls, mergeable_to_merger);
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
    std::unordered_map<const DexType*, DontMergeState>* dont_merge_status) {
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
            record_dont_merge_state(get_array_type_or_self(insn->get_type()),
                                    STRICT,
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
              record_dont_merge_state(
                  get_array_type_or_self(field->get_class()),
                  CONDITIONAL,
                  dont_merge_status);
            }
          } else {
            record_dont_merge_state(
                get_array_type_or_self(insn->get_field()->get_class()),
                CONDITIONAL,
                dont_merge_status);
          }
        } else if (insn->has_method()) {
          DexMethod* meth =
              resolve_method(insn->get_method(), MethodSearch::Any);
          if (meth != nullptr) {
            types_to_check.emplace(meth->get_class());
            // Don't merge an abstract class if its method is invoked through
            // invoke-virtual, it means we might need to keep both method
            // in parent and child class, and need to face true-virtual
            // renaming issue.
            // TODO(suree404): oportunity to improve this.
            if (insn->opcode() == OPCODE_INVOKE_VIRTUAL &&
                meth->get_class() == insn->get_method()->get_class()) {
              DexClass* method_class = type_class(meth->get_class());
              if (method_class && is_abstract(method_class)) {
                record_dont_merge_state(
                    get_array_type_or_self(meth->get_class()),
                    CONDITIONAL,
                    dont_merge_status);
              }
            }
          }
          types_to_check.emplace(insn->get_method()->get_class());
        }

        for (auto type_to_check : types_to_check) {
          const DexType* self_type = get_array_type_or_self(type_to_check);
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
      if (cls && !is_abstract(cls)) {
        // If a type is referenced and not a abstract type then add it to
        // don't use this type as mergeable.
        record_dont_merge_state(type, CONDITIONAL, dont_merge_status);
      }
    }
  };
  walk::methods(scope, [&](DexMethod* method) {
    DexProto* proto = method->get_proto();
    const DexType* rtype = get_array_type_or_self(proto->get_rtype());
    check_method_sig(rtype, method);
    DexTypeList* args = proto->get_args();
    for (const DexType* it : args->get_type_list()) {
      const DexType* extracted_type = get_array_type_or_self(it);
      check_method_sig(extracted_type, method);
    }
  });
}

void record_referenced(
    const Scope& scope,
    std::unordered_map<const DexType*, DontMergeState>* dont_merge_status) {
  record_annotation(scope, dont_merge_status);
  record_code_reference(scope, dont_merge_status);
  record_method_signature(scope, dont_merge_status);
}

} // namespace

void VerticalMergingPass::run_pass(DexStoresVector& stores,
                                   ConfigFiles& /*cfg*/,
                                   PassManager& mgr) {
  auto scope = build_class_scope(stores);
  std::unordered_map<const DexType*, DontMergeState> dont_merge_status;
  record_referenced(scope, &dont_merge_status);
  XStoreRefs xstores(stores);
  std::unordered_map<DexClass*, DexClass*> mergeable_to_merger;
  collect_can_merge(scope, xstores, dont_merge_status, &mergeable_to_merger);

  mgr.set_metric("num_pair_to_merge", mergeable_to_merger.size());
  TRACE(PM, 1, "num_pair_to_merge %d\n", mergeable_to_merger.size());
}

static VerticalMergingPass s_pass;
