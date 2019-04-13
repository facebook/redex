/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodInline.h"

#include <algorithm>
#include <boost/algorithm/string/predicate.hpp>
#include <string>
#include <vector>
#include <map>
#include <set>

#include "ClassHierarchy.h"
#include "Deleter.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "Inliner.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "ReachableClasses.h"
#include "Resolver.h"
#include "VirtualScope.h"
#include "Walkers.h"

void MethodInlinePass::run_pass(DexStoresVector& stores,
                                ConfigFiles& /* conf */,
                                PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(SINL, 1, "MethodInlinePass not run because no ProGuard configuration was provided.");
    return;
  }
  auto scope = build_class_scope(stores);
  // gather all inlinable candidates
  auto methods = gather_non_virtual_methods(scope);

  populate_blacklist(scope);

  auto resolver = [this](DexMethodRef* method, MethodSearch search) {
    return resolve_method(method, search, resolved_refs);
  };

  if (m_inliner_config.use_cfg_inliner) {
    walk::parallel::code(scope, [](DexMethod*, IRCode& code) {
      code.build_cfg(/* editable */ true);
    });
  }

  // inline candidates
  MultiMethodInliner inliner(scope, stores, methods, resolver,
                             m_inliner_config);
  inliner.inline_methods();

  if (m_inliner_config.use_cfg_inliner) {
    walk::parallel::code(scope, [](DexMethod*, IRCode& code) {
      code.clear_cfg();
    });
  }

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
std::unordered_set<DexMethod*> MethodInlinePass::gather_non_virtual_methods(
    Scope& scope) {
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
  size_t non_virt_dont_strip = 0;
  size_t non_virt_methods = 0;

  // collect all non virtual methods (dmethods and vmethods)
  std::unordered_set<DexMethod*> methods;

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

        methods.insert(method);
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
      methods.insert(vmeth);
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
  TRACE(SINL, 2, "Don't strip inlinable methods count: %ld\n", dont_strip);
  return methods;
}

void MethodInlinePass::populate_blacklist(const Scope& scope) {
  walk::classes(scope, [this](const DexClass* cls) {
    for (const auto& type_s : m_black_list) {
      if (boost::starts_with(cls->get_name()->c_str(), type_s)) {
        m_inliner_config.black_list.emplace(cls->get_type());
        break;
      }
    }
    for (const auto& type_s : m_caller_black_list) {
      if (boost::starts_with(cls->get_name()->c_str(), type_s)) {
        m_inliner_config.caller_black_list.emplace(cls->get_type());
        break;
      }
    }
  });
}

static MethodInlinePass s_pass;
