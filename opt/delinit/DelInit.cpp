/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DelInit.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

#include "walkers.h"
#include "DexClass.h"
#include "DexOpcode.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "ReachableClasses.h"

/*
 * This is not a visitor pattern dead-code eliminator with explicit
 * entry points.  Rather it's a delete everything that's never
 * referenced elimnator.  Thus the name "delinit".
 */

namespace {

/**
 * Return true for classes that should not be processed by the optimization.
 */
bool filter_class(DexClass* clazz) {
  always_assert(!clazz->is_external());
  return is_interface(clazz) || is_annotation(clazz) || !can_delete(clazz);
}

using ClassSet = std::unordered_set<DexClass*>;
using MethodSet = std::unordered_set<DexMethod*>;
using FieldSet = std::unordered_set<DexField*>;
using MethodVector = std::vector<DexMethod*>;

/**
 * Main class to track DelInit optimizations.
 * For each pass collects all the instance data (vmethods and ifields)
 * for classes that have no ctor or all unreachable ctors.
 * Then it walks all the opcodes to see if there are references to any of those
 * members and if so the member (method or field) is not deleted.
 * In the process it also finds all the methods and ctors unreachable.
 * Repeat the process until no more methods are removed.
 */
struct DeadRefs {
  // all the data is per pass, so it is cleared at the proper time
  // in each step.

  // list of classes that have no reachable ctor
  ClassSet classes;
  // list of vmethods from classes with no reachable ctor
  MethodSet vmethods;
  // list of ifields from classes with no reachable ctor
  FieldSet ifields;
  // set of invoked methods
  MethodSet called;
  // set of all ctors that are known
  MethodVector initmethods;
  // set of dmethods (no init or clinit) that are known
  MethodVector dmethods;

  // statistic info
  struct stats {
    size_t deleted_inits{0};
    size_t deleted_vmeths{0};
    size_t deleted_ifields{0};
    size_t deleted_dmeths{0};
  } del_init_res;

  void delinit(Scope& scope);
  int find_new_unreachable(Scope& scope);
  void find_unreachable(Scope& scope);
  void find_unreachable_data(DexClass* clazz);
  void collect_dmethods(Scope& scope);
  void track_callers(Scope& scope);
  int remove_unreachable();
};

/**
 * Entry point for DelInit.
 * Loop through the different steps until no more methods are deleted.
 */
void DeadRefs::delinit(Scope& scope) {
  int removed = 0;
  int passnum = 0;
  do {
    passnum++;
    TRACE(DELINIT, 2, "Summary for pass %d\n", passnum);
    removed = find_new_unreachable(scope);
    collect_dmethods(scope);
    track_callers(scope);
    removed += remove_unreachable();
  } while (removed > 0);
}

/**
 * Find new unreachable classes.
 * First it deletes all unreachable ctor then calls into find_unreachable.
 */
int DeadRefs::find_new_unreachable(Scope& scope) {
  int init_deleted = 0;
  int init_called = 0;
  int init_cant_delete = 0;
  int init_class_cant_delete = 0;
  for (auto init : initmethods) {
    if (called.count(init) > 0) {
      init_called++;
      continue;
    }
    if (!can_delete(init)) {
      init_cant_delete++;
      continue;
    }
    auto clazz = type_class(init->get_class());
    if (!can_delete(clazz)) {
      init_class_cant_delete++;
      continue;
    }
    clazz->get_dmethods().remove(init);
    TRACE(DELINIT, 5, "Delete init %s.%s %s\n", SHOW(init->get_class()),
        SHOW(init->get_name()), SHOW(init->get_proto()));
    init_deleted++;
  }
  TRACE(DELINIT, 2, "Removed %d <init> methods\n", init_deleted);
  TRACE(DELINIT, 3, "%d <init> methods called\n", init_called);
  TRACE(DELINIT, 3, "%d <init> methods do not delete\n", init_cant_delete);
  TRACE(DELINIT, 3, "%d <init> method classes do not delete\n",
      init_class_cant_delete);
  find_unreachable(scope);
  del_init_res.deleted_inits += init_deleted;
  return init_deleted;
}

/* Collect instance data for classes that do not have <init> routines.
 * This means the vtable and the ifields.
 */
void DeadRefs::find_unreachable(Scope& scope) {
  classes.clear();
  vmethods.clear();
  ifields.clear();
  for (auto clazz : scope) {
    if (filter_class(clazz)) continue;

    auto const& dmeths = clazz->get_dmethods();
    bool hasInit = false;
    for (auto meth : dmeths) {
      if (is_init(meth)) {
        hasInit = true;
        break;
      }
    }
    if (hasInit) continue;

    find_unreachable_data(clazz);
  }
  TRACE(DELINIT, 2,
      "Uninstantiable classes %ld: vmethods %ld, ifields %ld\n",
      classes.size(), vmethods.size(), ifields.size());
}

/**
 * Collect all instance data (ifields, vmethods) given the class is
 * uninstantiable.
 */
void DeadRefs::find_unreachable_data(DexClass* clazz) {
  classes.insert(clazz);

  for (const auto& meth : clazz->get_vmethods()) {
    if (!can_delete(meth)) continue;
    vmethods.insert(meth);
  }

  for (const auto& field : clazz->get_ifields()) {
    if (!can_delete(field)) continue;
    ifields.insert(field);
  }
}

/**
 * Collect all init and direct methods but not vm methods (clint, '<...').
 */
void DeadRefs::collect_dmethods(Scope& scope) {
  initmethods.clear();
  dmethods.clear();
  for (auto clazz : scope) {
    if (filter_class(clazz)) continue;

    auto const& dmeths = clazz->get_dmethods();
    for (auto meth : dmeths) {
      if (meth->get_code() == nullptr) continue;
      if (is_init(meth)) {
        initmethods.push_back(meth);
      } else {
        // Method names beginning with '<' are internal VM calls
        // except <init>
        if (meth->get_name()->c_str()[0] != '<') {
          dmethods.push_back(meth);
        }
      }
    }
  }
  TRACE(DELINIT, 3,
      "Found %ld init and %ld dmethods\n",
      initmethods.size(), dmethods.size());
}

/**
 * Walk all opcodes and find all methods called (live in scope).
 * Also remove all potentially unreachable members - if a reference exists -
 * from the set of removable instance data.
 */
void DeadRefs::track_callers(Scope& scope) {
  called.clear();
  walk_opcodes(scope,
      [](DexMethod*) { return true; },
      [&](DexMethod* m, DexOpcode* opcode) {
        if (opcode->has_methods()) {
          auto methodop = static_cast<DexOpcodeMethod*>(opcode);
          auto callee = methodop->get_method();
          callee = resolve_method(callee, opcode_to_search(opcode));
          if (callee == nullptr || !callee->is_concrete()) return;
          if (vmethods.count(callee) > 0) {
            vmethods.erase(callee);
          }
          called.insert(callee);
          return;
        }
        if (opcode->has_fields()) {
          auto fieldop = static_cast<DexOpcodeField*>(opcode);
          auto field = fieldop->field();
          field = resolve_field(field,
              is_ifield_op(opcode->opcode()) ?
                  FieldSearch::Instance :
                  is_sfield_op(opcode->opcode()) ?
                      FieldSearch::Static : FieldSearch::Any);
          if (field == nullptr || !field->is_concrete()) return;
          if (ifields.count(field) > 0) {
            ifields.erase(field);
          }
          return;
        }
      });
  TRACE(DELINIT, 3,
      "Unreachable (not called) %ld vmethods and %ld ifields\n",
      vmethods.size(), ifields.size());
}

/**
 * Delete of all unreachable members.
 */
int DeadRefs::remove_unreachable() {
  int vmethodcnt = 0;
  int dmethodcnt = 0;
  int ifieldcnt = 0;
  for (const auto& meth : vmethods) {
    assert(meth->is_virtual());
    auto cls = type_class(meth->get_class());
    auto& methods = cls->get_vmethods();
    auto meth_it = std::find(methods.begin(), methods.end(), meth);
    if (meth_it == methods.end()) continue;

    methods.erase(meth_it);
    vmethodcnt++;
    TRACE(DELINIT, 6, "Delete vmethod: %s.%s %s\n",
        SHOW(meth->get_class()), SHOW(meth->get_name()),
        SHOW(meth->get_proto()));
  }
  del_init_res.deleted_vmeths += vmethodcnt;
  TRACE(DELINIT, 2, "Removed %d vmethods\n", vmethodcnt);

  for (const auto& field : ifields) {
    assert(!is_static(field));
    auto cls = type_class(field->get_class());
    auto& fields = cls->get_ifields();
    auto field_it = std::find(fields.begin(), fields.end(), field);
    if (field_it == fields.end()) continue;

    fields.erase(field_it);
    ifieldcnt++;
    TRACE(DELINIT, 6, "Delete ifield: %s.%s %s\n",
      SHOW(field->get_class()), SHOW(field->get_name()),
      SHOW(field->get_type()));
  }
  del_init_res.deleted_ifields += ifieldcnt;
  TRACE(DELINIT, 2, "Removed %d ifields\n", ifieldcnt);

  int called_dmeths = 0;
  int dont_delete_dmeths = 0;
  for (const auto& meth : dmethods) {
    assert(!meth->is_virtual());
    if (called.count(meth) > 0) {
      called_dmeths++;
      continue;
    }
    if (!can_delete(meth)) {
      dont_delete_dmeths++;
      continue;
    }
    auto clazz = type_class(meth->get_class());
    clazz->get_dmethods().remove(meth);
    dmethodcnt++;
    TRACE(DELINIT, 6, "Delete dmethod: %s.%s %s\n",
        SHOW(meth->get_class()), SHOW(meth->get_name()),
        SHOW(meth->get_proto()));
  }
  del_init_res.deleted_dmeths += dmethodcnt;
  TRACE(DELINIT, 2, "Removed %d dmethods\n", dmethodcnt);
  TRACE(DELINIT, 3, "%d called dmethods\n", called_dmeths);
  TRACE(DELINIT, 3, "%d don't delete dmethods\n", dont_delete_dmeths);

  return vmethodcnt + ifieldcnt + dmethodcnt;
}

}

void DelInitPass::run_pass(DexClassesVector& dexen, PgoFiles& pgo) {
  auto scope = build_class_scope(dexen);
  DeadRefs drefs;
  drefs.delinit(scope);
  TRACE(DELINIT, 1, "Removed %d <init> methods\n",
      drefs.del_init_res.deleted_inits);
  TRACE(DELINIT, 1, "Removed %d vmethods\n",
      drefs.del_init_res.deleted_vmeths);
  TRACE(DELINIT, 1, "Removed %d ifields\n",
      drefs.del_init_res.deleted_ifields);
  TRACE(DELINIT, 1, "Removed %d dmethods\n",
      drefs.del_init_res.deleted_dmeths);
  post_dexen_changes(scope, dexen);
}
