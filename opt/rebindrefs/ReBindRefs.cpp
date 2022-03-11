/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
#include "ConfigFiles.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "PassManager.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
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

inline bool match(const DexString* name,
                  const DexProto* proto,
                  const DexMethod* cls_meth) {
  return name == cls_meth->get_name() && proto == cls_meth->get_proto();
}

// Only looking at the public, protected and private bits.
template <typename DexMember>
DexAccessFlags get_visibility(const DexMember* member) {
  return member->get_access() & VISIBILITY_MASK;
}

struct Rebinder {
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

    void print(const char* tag, PassManager& mgr) const {
      TRACE(BIND, 1, "%11s [call sites: %6d, old refs: %6lu, new refs: %6lu]",
            tag, count, in.size(), out.size());

      using std::string;
      string tagStr{tag};
      string count_metric = tagStr + string("_candidates");
      string rebound_metric = tagStr + string("_rebound");
      mgr.incr_metric(count_metric, count);

      auto rebound =
          static_cast<ssize_t>(in.size()) - static_cast<ssize_t>(out.size());
      mgr.incr_metric(rebound_metric, rebound);
    }

    RefStats& operator+=(const RefStats& rhs) {
      count += rhs.count;
      if (!rhs.in.empty()) {
        in.insert(rhs.in.begin(), rhs.in.end());
      }
      if (!rhs.out.empty()) {
        out.insert(rhs.out.begin(), rhs.out.end());
      }
      return *this;
    }
  };

 public:
  struct RebinderRefs {
    RefStats<DexMethodRef*> mrefs;
    RefStats<DexMethodRef*> array_clone_refs;

    void print(PassManager& mgr) const {
      mrefs.print("method_refs", mgr);
      array_clone_refs.print("array_clone", mgr);
    }

    RebinderRefs& operator+=(const RebinderRefs& rhs) {
      mrefs += rhs.mrefs;
      array_clone_refs += rhs.array_clone_refs;
      return *this;
    }
  };

  Rebinder(Scope& scope,
           bool rebind_to_external,
           std::vector<std::string>& excluded_externals,
           const api::AndroidSDK& min_sdk_sdk)
      : m_scope(scope),
        m_rebind_to_external(rebind_to_external),
        m_excluded_externals(excluded_externals),
        m_min_sdk_sdk(min_sdk_sdk) {}

  RebinderRefs rewrite_refs() {
    return walk::parallel::methods<RebinderRefs>(
        m_scope, [&](DexMethod* method) {
          if (!method || !method->get_code()) {
            return RebinderRefs();
          }
          bool is_support_lib = api::is_support_lib_type(method->get_class());
          RebinderRefs rebinder_refs;
          for (auto& mie : InstructionIterable(method->get_code())) {
            auto insn = mie.insn;
            switch (insn->opcode()) {
            case OPCODE_INVOKE_VIRTUAL: {
              rebind_invoke_virtual(is_support_lib, insn, rebinder_refs);
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
          return rebinder_refs;
        });
  }

 private:
  /**
   * Java allows relaxing visibility down the hierarchy chain so while
   * rebinding we don't want to bind to a method up the hierarchy that would
   * not be visible.
   * Walk up the hierarchy chain as long as the method is visible.
   */
  DexMethod* bind_to_visible_ancestor(const DexClass* cls,
                                      const DexString* name,
                                      const DexProto* proto) {
    auto leaf_impl = resolve_virtual(cls, name, proto);
    if (!leaf_impl) {
      return nullptr;
    }
    auto leaf_vis = get_visibility(leaf_impl);
    if (!is_public(leaf_vis)) {
      return leaf_impl;
    }
    DexMethod* top_impl = leaf_impl;
    // The resolved leaf impl can only be PUBLIC at this point.
    while (cls) {
      for (const auto& cls_meth : cls->get_vmethods()) {
        if (match(name, proto, cls_meth)) {
          auto curr_vis = get_visibility(cls_meth);
          auto curr_cls_vis = get_visibility(cls);
          if (is_private(curr_vis) || is_package_private(curr_vis)) {
            return top_impl;
          }
          bool is_external = cls->is_external() || cls_meth->is_external();
          if (is_external &&
              (!is_public(curr_vis) || !is_public(curr_cls_vis))) {
            return top_impl;
          }
          // We can only rebind PUBLIC to PUBLIC here.
          if (leaf_vis != curr_vis) {
            return top_impl;
          }
          top_impl = cls_meth;
          break;
        }
      }
      cls = type_class(cls->get_super_class());
    }
    return top_impl;
  }

  void rebind_invoke_virtual(bool is_support_lib,
                             IRInstruction* mop,
                             RebinderRefs& rebinder_refs) {
    const auto mref = mop->get_method();
    auto mtype = mref->get_class();
    if (is_array_clone(mref, mtype)) {
      rebind_method_opcode(is_support_lib, mop, mref,
                           rebind_array_clone(mref, rebinder_refs),
                           rebinder_refs);
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
    rebind_method_opcode(is_support_lib, mop, mref, real_ref, rebinder_refs);
  }

  bool is_excluded_external(const std::string& name) {
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
                            DexMethodRef* real_ref,
                            RebinderRefs& rebinder_refs) {
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
    if (is_external && real_ref->is_def()) {
      auto target_def = real_ref->as_def();
      if (!m_min_sdk_sdk.has_method(target_def)) {
        TRACE(BIND, 4, "Bailed on mismatch with min_sdk %s", SHOW(target_def));
        return;
      }
    }
    // Bail out if the def is non public external
    if (is_external && !is_public(cls)) {
      TRACE(BIND, 4, "non-public external %s", SHOW(real_ref));
      return;
    }
    TRACE(BIND, 2, "Rebinding %s\n\t=>%s", SHOW(mref), SHOW(real_ref));
    rebinder_refs.mrefs.insert(mref, real_ref);
    mop->set_method(real_ref);
    if (cls != nullptr && !is_public(cls)) {
      always_assert(!is_external);
      set_public(cls);
    }
  }

  DexMethodRef* rebind_array_clone(DexMethodRef* mref,
                                   RebinderRefs& rebinder_refs) {
    DexMethodRef* real_ref = object_array_clone();
    rebinder_refs.array_clone_refs.insert(mref, real_ref);
    return real_ref;
  }

  Scope& m_scope;
  bool m_rebind_to_external;
  std::vector<std::string> m_excluded_externals;
  const api::AndroidSDK& m_min_sdk_sdk;
};

} // namespace

void ReBindRefsPass::run_pass(DexStoresVector& stores,
                              ConfigFiles& /* conf */,
                              PassManager& mgr) {

  always_assert(m_min_sdk_api);
  Scope scope = build_class_scope(stores);
  Rebinder rb(scope, m_refine_to_external, m_excluded_externals,
              *m_min_sdk_api);
  auto stats = rb.rewrite_refs();
  stats.print(mgr);
}

static ReBindRefsPass s_pass;
