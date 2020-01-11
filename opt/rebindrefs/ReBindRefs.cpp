/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReBindRefs.h"

#include <boost/algorithm/string.hpp>
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

bool is_array_clone(DexMethodRef* mref, DexType* mtype) {
  static auto clone = DexString::make_string("clone");
  return type::is_array(mtype) && mref->get_name() == clone &&
         !type::is_primitive(type::get_array_element_type(mtype));
}

struct Rebinder {
  Rebinder(Scope& scope,
           PassManager& mgr,
           bool rebind_to_external,
           std::vector<std::string>& excluded_externals)
      : m_scope(scope),
        m_pass_mgr(mgr),
        m_rebind_to_external(rebind_to_external),
        m_excluded_externals(excluded_externals) {}

  void rewrite_refs() {
    walk::parallel::methods(m_scope, [&](DexMethod* method) {
      if (!method || !method->get_code()) {
        return;
      }
      bool is_support_lib = api::is_support_lib_type(method->get_class());
      for (auto& mie : InstructionIterable(method->get_code())) {
        auto insn = mie.insn;
        switch (insn->opcode()) {
        case OPCODE_INVOKE_VIRTUAL: {
          rebind_invoke_virtual(is_support_lib, insn);
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
      }
    });
  }

  void print_stats() {
    m_mrefs.print("method_refs", &m_pass_mgr);
    m_array_clone_refs.print("array_clone", &m_pass_mgr);
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

      using std::string;
      string tagStr{tag};
      string count_metric = tagStr + string("_candidates");
      string rebound_metric = tagStr + string("_rebound");
      mgr->incr_metric(count_metric, count);

      auto rebound =
          static_cast<ssize_t>(in.size()) - static_cast<ssize_t>(out.size());
      mgr->incr_metric(rebound_metric, rebound);
    }
  };

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
            auto top_cls_vis = type_class(top_impl->get_class())->get_access() &
                               VISIBILITY_MASK;
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

  void rebind_invoke_virtual(bool is_support_lib, IRInstruction* mop) {
    const auto mref = mop->get_method();
    auto mtype = mref->get_class();
    if (is_array_clone(mref, mtype)) {
      rebind_method_opcode(is_support_lib, mop, mref, rebind_array_clone(mref));
      return;
    }
    // leave java.lang.String alone not to interfere with OP_EXECUTE_INLINE
    // and possibly any smart handling of String
    if (mtype == type::java_lang_String()) {
      return;
    }
    auto cls = type_class(mtype);
    auto real_ref =
        bind_to_visible_ancestor(cls, mref->get_name(), mref->get_proto());
    rebind_method_opcode(is_support_lib, mop, mref, real_ref);
  }

  bool is_excluded_external(std::string name) {
    for (auto& excluded : m_excluded_externals) {
      if (boost::starts_with(name, excluded)) {
        return true;
      }
    }

    return false;
  }

  void rebind_method_opcode(bool is_support_lib,
                            IRInstruction* mop,
                            DexMethodRef* mref,
                            DexMethodRef* real_ref) {
    if (!real_ref || real_ref == mref) {
      return;
    }
    auto cls = type_class(real_ref->get_class());
    bool is_external = cls && cls->is_external();
    if (is_external && !m_rebind_to_external) {
      TRACE(BIND, 4, "external %s", SHOW(real_ref));
      return;
    }
    // If the rebind target is in the exluded external list, stop.
    if (is_external && is_excluded_external(show(real_ref))) {
      TRACE(BIND, 4, "excluded external %s", SHOW(real_ref));
      return;
    }
    // If the caller is in support libraries Android support library or Android
    // X, we don't rebind if the target is in Android SDK. That means we do
    // rebind for JDK classes since we know it's safe to do so.
    if (is_external && api::is_android_sdk_type(real_ref->get_class()) &&
        is_support_lib) {
      TRACE(BIND, 4, "support lib external %s", SHOW(real_ref));
      return;
    }
    // Bail out if the def is non public external
    if (is_external && !is_public(cls)) {
      TRACE(BIND, 4, "non-public external %s", SHOW(real_ref));
      return;
    }
    TRACE(BIND, 2, "Rebinding %s\n\t=>%s", SHOW(mref), SHOW(real_ref));
    m_mrefs.insert(mref, real_ref);
    mop->set_method(real_ref);
    if (cls != nullptr && !is_public(cls)) {
      set_public(cls);
    }
  }

  DexMethodRef* rebind_array_clone(DexMethodRef* mref) {
    DexMethodRef* real_ref = object_array_clone();
    m_array_clone_refs.insert(mref, real_ref);
    return real_ref;
  }

  Scope& m_scope;
  PassManager& m_pass_mgr;
  bool m_rebind_to_external;
  std::vector<std::string> m_excluded_externals;

  RefStats<DexMethodRef*> m_mrefs;
  RefStats<DexMethodRef*> m_array_clone_refs;
};

} // namespace

void ReBindRefsPass::run_pass(DexStoresVector& stores,
                              ConfigFiles& /* conf */,
                              PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  Rebinder rb(scope, mgr, m_rebind_to_external, m_excluded_externals);
  rb.rewrite_refs();
  rb.print_stats();
}

static ReBindRefsPass s_pass;
