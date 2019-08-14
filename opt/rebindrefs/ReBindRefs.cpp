/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReBindRefs.h"

#include <functional>
#include <string>
#include <vector>

#include "ApiLevelChecker.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Util.h"
#include "Walkers.h"

namespace {

DexMethodRef* object_array_clone() {
  static DexMethodRef* clone = DexMethod::make_method(
      "[Ljava/lang/Object;", "clone", "Ljava/lang/Object;", {});
  return clone;
}

DexMethodRef* object_equals() {
  static DexMethodRef* equals = DexMethod::make_method(
      "Ljava/lang/Object;", "equals", "Z", {"Ljava/lang/Object;"});
  return equals;
}

DexMethodRef* object_hashCode() {
  static DexMethodRef* hashCode =
      DexMethod::make_method("Ljava/lang/Object;", "hashCode", "I", {});
  return hashCode;
}

DexMethodRef* object_getClass() {
  static DexMethodRef* getClass = DexMethod::make_method(
      "Ljava/lang/Object;", "getClass", "Ljava/lang/Class;", {});
  return getClass;
}

bool is_object_equals(DexMethodRef* mref) {
  static DexString* equals = DexString::make_string("equals");
  static DexProto* bool_obj =
      DexProto::make_proto(DexType::make_type("Z"),
                           DexTypeList::make_type_list({get_object_type()}));
  return mref->get_name() == equals && mref->get_proto() == bool_obj;
}

bool is_object_hashCode(DexMethodRef* mref) {
  static DexString* hashCode = DexString::make_string("hashCode");
  static DexProto* int_void =
      DexProto::make_proto(get_int_type(), DexTypeList::make_type_list({}));
  return mref->get_name() == hashCode && mref->get_proto() == int_void;
}

bool is_object_getClass(DexMethodRef* mref) {
  static DexString* getClass = DexString::make_string("getClass");
  static DexProto* cls_void =
      DexProto::make_proto(get_class_type(), DexTypeList::make_type_list({}));
  return mref->get_name() == getClass && mref->get_proto() == cls_void;
}

/**
 * Java allows relaxing visibility down the hierarchy chain so while
 * rebinding we don't want to bind to a method up the hierarchy that would
 * not be visible.
 * Walk up the hierarchy chain as long as the method is public.
 */
DexMethod* bind_to_visible_ancestor(const DexClass* cls,
                                    const DexString* name,
                                    const DexProto* proto) {
  DexMethod* top_impl = nullptr;
  while (cls) {
    for (const auto& cls_meth : cls->get_vmethods()) {
      if (name == cls_meth->get_name() && proto == cls_meth->get_proto()) {
        auto curr_vis = cls_meth->get_access() & VISIBILITY_MASK;
        auto curr_cls_vis = cls->get_access() & VISIBILITY_MASK;
        if (curr_vis != ACC_PUBLIC || curr_cls_vis != ACC_PUBLIC) {
          return top_impl != nullptr ? top_impl : cls_meth;
        }
        if (top_impl != nullptr) {
          auto top_vis = top_impl->get_access() & VISIBILITY_MASK;
          auto top_cls_vis =
              type_class(top_impl->get_class())->get_access() & VISIBILITY_MASK;
          if (top_vis != curr_vis || top_cls_vis != curr_cls_vis) {
            return top_impl;
          }
        }
        top_impl = cls_meth;
        break;
      }
    }
    cls = type_class(cls->get_super_class());
  }
  return top_impl;
}

struct Rebinder {
  Rebinder(Scope& scope, PassManager& mgr, bool rebind_to_external)
      : m_scope(scope),
        m_pass_mgr(mgr),
        m_rebind_to_external(rebind_to_external) {}

  void rewrite_refs() {
    walk::opcodes(m_scope,
                  [](DexMethod*) { return true; },
                  [&](DexMethod* m, IRInstruction* insn) {
                    bool top_ancestor = false;
                    switch (insn->opcode()) {
                    case OPCODE_INVOKE_VIRTUAL: {
                      rebind_invoke_virtual(insn);
                      break;
                    }
                    case OPCODE_INVOKE_SUPER:
                    case OPCODE_INVOKE_INTERFACE:
                    case OPCODE_INVOKE_STATIC: {
                      // Do nothing for now.
                      break;
                    }
                    default:
                      break;
                    }
                  });
  }

  void print_stats() {
    m_mrefs.print("method_refs", &m_pass_mgr);
    m_array_clone_refs.print("array_clone", nullptr);
    m_equals_refs.print("equals", nullptr);
    m_hashCode_refs.print("hashCode", nullptr);
    m_getClass_refs.print("getClass", nullptr);
  }

 private:
  template <typename T>
  struct RefStats {
    int count = 0;
    std::unordered_set<T> in;
    std::unordered_set<T> out;

    void insert(T tin, T tout) {
      ++count;
      in.emplace(tin);
      out.emplace(tout);
    }

    void insert(T tin) { insert(tin, T()); }

    void print(const char* tag, PassManager* mgr) {
      TRACE(BIND, 1, "%11s [call sites: %6d, old refs: %6lu, new refs: %6lu]",
            tag, count, in.size(), out.size());

      if (mgr) {
        using std::string;
        string tagStr{tag};
        string count_metric = tagStr + string("_candidates");
        string rebound_metric = tagStr + string("_rebound");
        mgr->incr_metric(count_metric, count);

        auto rebound =
            static_cast<ssize_t>(in.size()) - static_cast<ssize_t>(out.size());
        mgr->incr_metric(rebound_metric, rebound);
      }
    }
  };

  void rebind_invoke_virtual(IRInstruction* mop) {
    const auto mref = mop->get_method();
    auto mtype = mref->get_class();
    if (is_array_clone(mref, mtype)) {
      rebind_method_opcode(mop, mref, rebind_array_clone(mref));
      return;
    }
    // leave java.lang.String alone not to interfere with OP_EXECUTE_INLINE
    // and possibly any smart handling of String
    static auto str = DexType::make_type("Ljava/lang/String;");
    if (mtype == str) return;
    auto real_ref = rebind_object_methods(mref);
    if (real_ref) {
      rebind_method_opcode(mop, mref, real_ref);
      return;
    }
    // Similar to the conditions we have in ResolveRefs, if the existing ref is
    // external already we don't rebind to the top. We are likely going to screw
    // up delicated logic in Android support library or Android x that handles
    // different OS versions.
    if (references_external(mref)) {
      return;
    }
    auto cls = type_class(mtype);
    real_ref =
        bind_to_visible_ancestor(cls, mref->get_name(), mref->get_proto());
    rebind_method_opcode(mop, mref, real_ref);
  }

  void rebind_method_opcode(IRInstruction* mop,
                            DexMethodRef* mref,
                            DexMethodRef* real_ref) {
    if (!real_ref || real_ref == mref) {
      return;
    }
    if (!m_rebind_to_external && real_ref->is_external()) {
      return;
    }
    // If the top def is an Android SDK type, we give up.
    // We know that the original method ref is internal thanks to the
    // references_external check above. It's risky to rebind it to the Android
    // SDK type. We may rebind it to a class that only exists in the OS version
    // we are compiling against but does not in older OS versions. It's safe to
    // avoid the complication.
    if (api::is_android_sdk_type(real_ref->get_class())) {
      return;
    }
    auto cls = type_class(real_ref->get_class());
    // Bail out if the def is non public external
    if (cls && cls->is_external() && !is_public(cls)) {
      return;
    }
    TRACE(BIND, 2, "Rebinding %s\n\t=>%s", SHOW(mref), SHOW(real_ref));
    m_mrefs.insert(mref, real_ref);
    mop->set_method(real_ref);
    if (cls != nullptr && !is_public(cls)) {
      set_public(cls);
    }
  }

  bool is_array_clone(DexMethodRef* mref, DexType* mtype) {
    static auto clone = DexString::make_string("clone");
    return is_array(mtype) && mref->get_name() == clone &&
           !is_primitive(get_array_element_type(mtype));
  }

  DexMethodRef* rebind_array_clone(DexMethodRef* mref) {
    DexMethodRef* real_ref = object_array_clone();
    m_array_clone_refs.insert(mref, real_ref);
    return real_ref;
  }

  DexMethodRef* rebind_object_methods(DexMethodRef* mref) {
    if (is_object_equals(mref)) {
      m_equals_refs.insert(mref);
      return object_equals();
    } else if (is_object_hashCode(mref)) {
      m_hashCode_refs.insert(mref);
      return object_hashCode();
    } else if (is_object_getClass(mref)) {
      m_getClass_refs.insert(mref);
      return object_getClass();
    }
    return nullptr;
  }

  Scope& m_scope;
  PassManager& m_pass_mgr;
  bool m_rebind_to_external;

  RefStats<DexMethodRef*> m_mrefs;
  RefStats<DexMethodRef*> m_array_clone_refs;
  RefStats<DexMethodRef*> m_equals_refs;
  RefStats<DexMethodRef*> m_hashCode_refs;
  RefStats<DexMethodRef*> m_getClass_refs;
};

} // namespace

void ReBindRefsPass::run_pass(DexStoresVector& stores,
                              ConfigFiles& /* conf */,
                              PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  Rebinder rb(scope, mgr, m_rebind_to_external);
  rb.rewrite_refs();
  rb.print_stats();
}

static ReBindRefsPass s_pass;
