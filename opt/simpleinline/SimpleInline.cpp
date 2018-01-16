/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <algorithm>
#include <string>
#include <vector>
#include <map>
#include <set>

#include "SimpleInline.h"
#include "Inliner.h"
#include "Deleter.h"
#include "DexClass.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "Walkers.h"
#include "ReachableClasses.h"
#include "VirtualScope.h"
#include "ClassHierarchy.h"

namespace {

std::unordered_set<DexType*> no_inline_annos(
  const std::vector<std::string>& annos,
  ConfigFiles& cfg
) {
  std::unordered_set<DexType*> no_inline;
  for (const auto& anno : cfg.get_no_optimizations_annos()) {
    no_inline.insert(anno);
  }
  for (auto const& no_inline_anno : annos) {
    auto type = DexType::get_type(
        DexString::get_string(no_inline_anno.c_str()));
    if (type != nullptr) {
      no_inline.insert(type);
    }
  }
  return no_inline;
}

std::unordered_set<DexType*> force_inline_annos(
    const std::vector<std::string>& annos) {
  std::unordered_set<DexType*> force_inline;
  for (auto const& force_inline_anno : annos) {
    auto type = DexType::get_type(force_inline_anno.c_str());
    if (type != nullptr) {
      force_inline.insert(type);
    }
  }
  return force_inline;
}

template<typename DexMember>
bool has_anno(DexMember* m, const std::unordered_set<DexType*>& no_inline) {
  if (no_inline.size() == 0) return false;
  if (m != nullptr && m->get_anno_set() != nullptr) {
    for (const auto& anno : m->get_anno_set()->get_annotations()) {
      if (no_inline.count(anno->type()) > 0) {
        return true;
      }
    }
  }
  return false;
}

}

void SimpleInlinePass::run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(SINL, 1, "SimpleInlinePass not run because no ProGuard configuration was provided.");
    return;
  }
  const auto no_inline = no_inline_annos(m_no_inline_annos, cfg);
  const auto force_inline = force_inline_annos(m_force_inline_annos);

  auto scope = build_class_scope(stores);
  // gather all inlinable candidates
  auto methods = gather_non_virtual_methods(scope, no_inline, force_inline);
  select_inlinable(
      scope, methods, resolved_refs, &inlinable, m_multiple_callers);

  auto resolver = [&](DexMethodRef* method, MethodSearch search) {
    return resolve_method(method, search, resolved_refs);
  };

  // inline candidates
  MultiMethodInliner inliner(
      scope, stores, inlinable, resolver, m_inliner_config);
  inliner.inline_methods();

  // delete all methods that can be deleted
  auto inlined = inliner.get_inlined();
  size_t inlined_count = inlined.size();
  size_t deleted = delete_methods(scope, inlined, resolver);

  TRACE(SINL, 3, "recursive %ld\n", inliner.get_info().recursive);
  TRACE(SINL, 3, "blacklisted meths %ld\n", inliner.get_info().blacklisted);
  TRACE(SINL, 3, "virtualizing methods %ld\n", inliner.get_info().need_vmethod);
  TRACE(SINL, 3, "invoke super %ld\n", inliner.get_info().invoke_super);
  TRACE(SINL, 3, "override inputs %ld\n", inliner.get_info().write_over_ins);
  TRACE(SINL, 3, "escaped virtual %ld\n", inliner.get_info().escaped_virtual);
  TRACE(SINL, 3, "known non public virtual %ld\n",
      inliner.get_info().non_pub_virtual);
  TRACE(SINL, 3, "non public ctor %ld\n", inliner.get_info().non_pub_ctor);
  TRACE(SINL, 3, "unknown field %ld\n", inliner.get_info().escaped_field);
  TRACE(SINL, 3, "non public field %ld\n", inliner.get_info().non_pub_field);
  TRACE(SINL, 3, "throws %ld\n", inliner.get_info().throws);
  TRACE(SINL, 3, "multiple returns %ld\n", inliner.get_info().multi_ret);
  TRACE(SINL, 3, "references cross stores %ld\n",
      inliner.get_info().cross_store);
  TRACE(SINL, 3, "not found %ld\n", inliner.get_info().not_found);
  TRACE(SINL, 3, "caller too large %ld\n", inliner.get_info().caller_too_large);
  TRACE(SINL, 1,
      "%ld inlined calls over %ld methods and %ld methods removed\n",
      inliner.get_info().calls_inlined, inlined_count, deleted);

  mgr.incr_metric("calls_inlined", inliner.get_info().calls_inlined);
  mgr.incr_metric("methods_removed", deleted);
}

/**
 * Collect all non virtual methods and make all small methods candidates
 * for inlining.
 */
std::unordered_set<DexMethod*> SimpleInlinePass::gather_non_virtual_methods(
    Scope& scope,
    const std::unordered_set<DexType*>& no_inline,
    const std::unordered_set<DexType*>& force_inline) {
  // trace counter
  size_t all_methods = 0;
  size_t direct_methods = 0;
  size_t direct_no_code = 0;
  size_t non_virtual_no_code = 0;
  size_t clinit = 0;
  size_t init = 0;
  size_t static_methods = 0;
  size_t private_methods = 0;
  size_t dont_strip = 0;
  size_t no_inline_anno_count = 0;
  size_t non_virt_dont_strip = 0;
  size_t non_virt_methods = 0;

  // collect all non virtual methods (dmethods and vmethods)
  std::unordered_set<DexMethod*> methods;

  const auto can_inline_method = [&](DexMethod* meth, const IRCode& code) {
    DexClass* cls = type_class(meth->get_class());
    if (has_anno(cls, no_inline) ||
        has_anno(meth, no_inline)) {
      no_inline_anno_count++;
      return;
    }
    if (code.count_opcodes() < SMALL_CODE_SIZE) {
      // always inline small methods even if they are not deletable
      inlinable.insert(meth);
    } else {
      if (!can_delete(meth)) {
        // never inline methods that cannot be deleted
        TRACE(SINL, 4, "cannot_delete: %s\n", SHOW(meth));
        dont_strip++;
      } else {
        methods.insert(meth);
      }
    }

    if (has_anno(meth, force_inline)) {
      inlinable.insert(meth);
    }
  };

  walk::methods(scope,
      [&](DexMethod* method) {
        all_methods++;
        if (method->is_virtual()) return;

        auto code = method->get_code();
        bool dont_inline = code == nullptr;

        direct_methods++;
        if (code == nullptr) direct_no_code++;
        if (is_constructor(method)) {
          (is_static(method)) ? clinit++ : init++;
          dont_inline = true;
        } else {
          (is_static(method)) ? static_methods++ : private_methods++;
        }

        if (dont_inline) return;

        can_inline_method(method, *code);
      });
  if (m_virtual_inline) {
    auto non_virtual = devirtualize(scope);
    non_virt_methods = non_virtual.size();
    for (const auto& vmeth : non_virtual) {
      auto code = vmeth->get_code();
      if (code == nullptr) {
        non_virtual_no_code++;
        continue;
      }
      can_inline_method(vmeth, *code);
    }
  }

  TRACE(SINL, 2, "All methods count: %ld\n", all_methods);
  TRACE(SINL, 2, "Direct methods count: %ld\n", direct_methods);
  TRACE(SINL, 2, "Virtual methods count: %ld\n", all_methods - direct_methods);
  TRACE(SINL, 2, "Direct methods no code: %ld\n", direct_no_code);
  TRACE(SINL, 2, "Direct methods with code: %ld\n",
      direct_methods - direct_no_code);
  TRACE(SINL, 2, "Constructors with or without code: %ld\n", init);
  TRACE(SINL, 2, "Static constructors: %ld\n", clinit);
  TRACE(SINL, 2, "Static methods: %ld\n", static_methods);
  TRACE(SINL, 2, "Private methods: %ld\n", private_methods);
  TRACE(SINL, 2, "Virtual methods non virtual count: %ld\n", non_virt_methods);
  TRACE(SINL, 2, "Non virtual no code count: %ld\n", non_virtual_no_code);
  TRACE(SINL, 2, "Non virtual no strip count: %ld\n", non_virt_dont_strip);
  TRACE(SINL, 2, "Small methods: %ld\n", inlinable.size());
  TRACE(SINL, 2, "Don't strip inlinable methods count: %ld\n", dont_strip);
  TRACE(SINL, 2, "Don't inline annotation count: %ld\n", no_inline_anno_count);
  return methods;
}

static SimpleInlinePass s_pass;
