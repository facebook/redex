/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "SimpleInline.h"
#include "InlineHelper.h"
#include "Deleter.h"
#include "DexClass.h"
#include "DexOpcode.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "Transform.h"
#include "walkers.h"
#include "ReachableClasses.h"
#include "Devirtualizer.h"

#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace {

// the max number of callers we care to track explicitly, after that we
// group all callees/callers count in the same bucket
const int MAX_COUNT = 10;

// sort and print a map of type or method to the count of their presence
// when inlining
template <typename Key>
DEBUG_ONLY bool sort_and_print(std::map<Key, int>& member_map,
                               int break_out,
                               const char* msg) {
  std::vector<std::pair<Key, int>> member_list;
  for (auto entry : member_map) {
    auto p = std::make_pair(entry.first, entry.second);
    member_list.push_back(p);
  }
  std::sort(member_list.begin(), member_list.end(),
      [](const std::pair<Key, int> v1, const std::pair<Key, int> v2) {
        return v1.second > v2.second;
      });
  TRACE(SINL, 5, "%s\n", msg);
  for (auto p : member_list) {
    if (p.second < break_out) break;
    TRACE(SINL, 5, "%s => %d\n", SHOW(p.first), p.second);
  }
  return true;
}

DEBUG_ONLY bool method_breakup(
    std::vector<std::vector<DexMethod*>>& calls_group) {
  size_t size = calls_group.size();
  for (size_t i = 0; i < size; ++i) {
    size_t inst = 0;
    size_t stat = 0;
    auto group = calls_group[i];
    for (auto callee : group) {
      callee->get_access() & ACC_STATIC ? stat++ : inst++;
    }
    TRACE(SINL, 5, "%ld callers %ld: instance %ld, static %ld\n",
        i, group.size(), inst, stat);
  }
  return true;
}

std::unordered_set<DexType*> no_inline_annos(
    const folly::dynamic& config, PgoFiles& pgo) {
  std::unordered_set<DexType*> no_inline;
  for (const auto& anno : pgo.get_no_optimizations_annos()) {
    no_inline.insert(anno);
  }
  if (!config.isObject()) return no_inline;
  auto it = config.find("no_inline_annos");
  if (it == config.items().end()) return no_inline;
  for (auto const& no_inline_anno : it->second) {
    auto type = DexType::get_type(
        DexString::get_string(no_inline_anno.c_str()));
    if (type != nullptr) {
      no_inline.insert(type);
    }
  }
  return no_inline;
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

void SimpleInlinePass::run_pass(DexClassesVector& dexen, PgoFiles& pgo) {
  const auto no_inline = no_inline_annos(m_config, pgo);

  virtual_inline = true;
  try {
    virtual_inline = m_config.at("virtual").asInt() == 1;
  } catch (...) {
    // Swallow exception if no configuration.
  }
  auto scope = build_class_scope(dexen);
  // gather all inlinable candidates
  auto methods = gather_non_virtual_methods(scope, no_inline);
  select_single_called(scope, methods);

  auto resolver = [&](DexMethod* method, MethodSearch search) {
    return resolve_method(method, search, resolved_refs);
  };

  // inline candidates
  MultiMethodInliner inliner(scope, dexen[0], inlinable, resolver);
  inliner.inline_methods();

  // delete all methods that can be deleted
  auto inlined = inliner.get_inlined();
  size_t inlined_count = inlined.size();
  size_t deleted = delete_methods(scope, inlined, resolver);

  TRACE(SINL, 3, "recursive %ld\n", inliner.get_info().recursive);
  TRACE(SINL, 3, "invoke range %ld\n", inliner.get_info().invoke_range);
  TRACE(SINL, 3, "enum meths %ld\n", inliner.get_info().enum_callee);
  TRACE(SINL, 3, "more than 16 regs %ld\n",
      inliner.get_info().more_than_16regs);
  TRACE(SINL, 3, "try/catch in callee %ld\n",
      inliner.get_info().try_catch_block);
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
  TRACE(SINL, 3, "array data %ld\n", inliner.get_info().array_data);
  TRACE(SINL, 3, "multiple returns %ld\n", inliner.get_info().multi_ret);
  TRACE(SINL, 3, "reference outside of primary %ld\n",
      inliner.get_info().not_in_primary);
  TRACE(SINL, 3, "not found %ld\n", inliner.get_info().not_found);
  // assert(sort_and_print(unk_refs, 50, "INVOKE OVER CLASSES"));
  // assert(sort_and_print(unk_meths, 5, "INVOKE OVER METHODS"));
  TRACE(SINL, 1,
      "%ld inlined calls over %ld methods and %ld methods removed\n",
      inliner.get_info().calls_inlined, inlined_count, deleted);
}

/**
 * Collect all non virtual methods and make all small methods candidates
 * for inlining.
 */
std::unordered_set<DexMethod*> SimpleInlinePass::gather_non_virtual_methods(
    Scope& scope, const std::unordered_set<DexType*>& no_inline) {
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

  const auto can_inline_method = [&](DexMethod* meth, DexCode* code) {
    if (has_anno(type_class(meth->get_class()), no_inline) ||
        has_anno(meth, no_inline)) {
      no_inline_anno_count++;
      return;
    }
    if (code->get_instructions().size() < SMALL_CODE_SIZE) {
      // always inline small methods even if they are not deletable
      inlinable.insert(meth);
    } else {
      if (do_not_strip(meth)) {
        // never inline methods that cannot be deleted
        dont_strip++;
      } else {
        methods.insert(meth);
      }
    }
  };

  walk_methods(scope,
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

        can_inline_method(method, code);
      });
  if (virtual_inline) {
    auto non_virtual = devirtualize(scope);
    non_virt_methods = non_virtual.size();
    for (const auto& vmeth : non_virtual) {
      auto code = vmeth->get_code();
      if (code == nullptr) {
        non_virtual_no_code++;
        continue;
      }
      can_inline_method(vmeth, code);
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

/**
 * Add to the list the single called.
 */
void SimpleInlinePass::select_single_called(
    Scope& scope, std::unordered_set<DexMethod*>& methods) {
  std::unordered_map<DexMethod*, int> calls;
  for (const auto& method : methods) {
    calls[method] = 0;
  }
  // count call sites for each method
  walk_opcodes(scope, [](DexMethod* meth) { return true; },
      [&](DexMethod* meth, DexOpcode* opcode) {
        if (is_invoke(opcode->opcode())) {
          auto mop = static_cast<DexOpcodeMethod*>(opcode);
          auto callee = resolve_method(
              mop->get_method(), opcode_to_search(opcode), resolved_refs);
          if (callee != nullptr && callee->is_concrete()
              && methods.count(callee) > 0) {
            calls[callee]++;
          }
        }
      });

  // pick methods with a single call site and add to candidates.
  // This vector usage is only because of logging we should remove it
  // once the optimization is "closed"
  std::vector<std::vector<DexMethod*>> calls_group(MAX_COUNT);
  for (auto call_it : calls) {
    if (call_it.second >= MAX_COUNT) {
      calls_group[MAX_COUNT - 1].push_back(call_it.first);
      continue;
    }
    calls_group[call_it.second].push_back(call_it.first);
  }
  assert(method_breakup(calls_group));
  for (auto callee : calls_group[1]) {
    inlinable.insert(callee);
  }
}
