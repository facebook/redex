/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Inliner.h"

#include "AnnoUtils.h"
#include "ApiLevelChecker.h"
#include "CFGInliner.h"
#include "ControlFlow.h"
#include "DexUtil.h"
#include "EditableCfgAdapter.h"
#include "IRInstruction.h"
#include "Mutators.h"
#include "OptData.h"
#include "Resolver.h"
#include "Transform.h"
#include "Walkers.h"

using namespace opt_metadata;

namespace {

const size_t CODE_SIZE_2_CALLERS = 7;
const size_t CODE_SIZE_3_CALLERS = 5;
// count of instructions that define a method as inlinable always
const size_t CODE_SIZE_ANY_CALLERS = 2;

// the max number of callers we care to track explicitly, after that we
// group all callees/callers count in the same bucket
const int MAX_COUNT = 10;

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

// add any type on which an access is allowed and safe without accessibility
// issues
const char* safe_types_on_refs[] = {
    "Ljava/lang/Object;",
    "Ljava/lang/String;",
    "Ljava/lang/Enum;",
    "Ljava/lang/StringBuilder;",
    "Ljava/lang/Boolean;",
    "Ljava/lang/Class;",
    "Ljava/lang/Long;",
    "Ljava/lang/Integer;",
    "Landroid/os/Bundle;",
    "Ljava/nio/ByteBuffer;"
};

/*
 * This is the maximum size of method that Dex bytecode can encode.
 * The table of instructions is indexed by a 32 bit unsigned integer.
 */
constexpr uint64_t HARD_MAX_INSTRUCTION_SIZE = 1L << 32;

/*
 * Some versions of ART (5.0.0 - 5.0.2) will fail to verify a method if it
 * is too large. See https://code.google.com/p/android/issues/detail?id=66655.
 *
 * The verifier rounds up to the next power of two, and doesn't support any
 * size greater than 16. See
 * http://androidxref.com/5.0.0_r2/xref/art/compiler/dex/verified_method.cc#107
 */
constexpr uint32_t SOFT_MAX_INSTRUCTION_SIZE = 1 << 15;
constexpr uint32_t INSTRUCTION_BUFFER = 1 << 12;

/**
 * Use this cache once the optimization is invoked to make sure
 * all caches are filled and usable.
 */
struct DexTypeCache {
  std::vector<DexType*> cache;

  DexTypeCache() {
    for (auto const safe_type : safe_types_on_refs) {
      auto type = DexType::get_type(safe_type);
      if (type != nullptr) {
        cache.push_back(type);
      }
    }
  }

  bool has_type(DexType* type) {
    for (auto cached_type : cache) {
      if (cached_type == type) return true;
    }
    return false;
  }
};

/**
 * If the type is a known final type or a well known type with no protected
 * methods the invocation is ok and can be optimized.
 * The problem here is that we don't have knowledge of all the types known
 * to the app and so we cannot determine whether the method was public or
 * protected. When public the optimization holds otherwise it's not always
 * possible to optimize and we conservatively give up.
 */
bool type_ok(DexType* type) {
  static DexTypeCache* cache = new DexTypeCache();
  return cache->has_type(type);
}

/**
 * If the method is a known public method over a known public class
 * the optimization is safe.
 * Following is a short list of safe methods that are called with frequency
 * and are optimizable.
 */
bool method_ok(DexType* type, DexMethodRef* meth) {
  auto meth_name = meth->get_name()->c_str();
  static auto view = DexType::get_type("Landroid/view/View;");
  if (view == type) {
    if (strcmp(meth_name, "getContext") == 0) return true;
    if (strcmp(meth_name, "findViewById") == 0) return true;
    if (strcmp(meth_name, "setVisibility") == 0) return true;
    return false;
  }
  static auto il = DexType::get_type(
      "Lcom/google/common/collect/ImmutableList;");
  static auto al = DexType::get_type("Ljava/util/ArrayList;");
  if (il == type || al == type) {
    if (strcmp(meth_name, "get") == 0) return true;
    if (strcmp(meth_name, "isEmpty") == 0) return true;
    if (strcmp(meth_name, "size") == 0) return true;
    if (strcmp(meth_name, "add") == 0) return true;
    return false;
  }
  static auto ctx = DexType::get_type("Landroid/content/Context;");
  if (ctx == type) {
    if (strcmp(meth_name, "getResources") == 0) return true;
    return false;
  }
  static auto resrc = DexType::get_type("Landroid/content/res/Resources;");
  if (resrc == type) {
    if (strcmp(meth_name, "getString") == 0) return true;
    return false;
  }
  static auto linf = DexType::get_type("Landroid/view/LayoutInflater;");
  if (linf == type) {
    if (strcmp(meth_name, "inflate") == 0) return true;
    return false;
  }
  static auto vg = DexType::get_type("Landroid/view/ViewGroup;");
  if (vg == type) {
    if (strcmp(meth_name, "getContext") == 0) return true;
    return false;
  }
  return false;
}

} // namespace

MultiMethodInliner::MultiMethodInliner(
    const std::vector<DexClass*>& scope,
    DexStoresVector& stores,
    const std::unordered_set<DexMethod*>& candidates,
    std::function<DexMethod*(DexMethodRef*, MethodSearch)> resolve_fn,
    const Config& config)
    : resolver(resolve_fn), xstores(stores), m_scope(scope), m_config(config) {
  // Walk every opcode in scope looking for calls to inlinable candidates and
  // build a map of callers to callees and the reverse callees to callers. If
  // within_dex is false, we build the map for all the candidates. If within_dex
  // is true, we properly exclude methods who have callers being located in
  // another dex from the candidates.
  if (m_config.within_dex) {
    std::unordered_set<DexMethod*> candidate_callees(candidates.begin(),
                                                     candidates.end());
    XDexRefs x_dex(stores);
    walk::opcodes(scope, [](DexMethod* caller) { return true; },
                  [&](DexMethod* caller, IRInstruction* insn) {
                    if (is_invoke(insn->opcode())) {
                      auto callee =
                          resolver(insn->get_method(), opcode_to_search(insn));
                      if (callee != nullptr && callee->is_concrete() &&
                          candidate_callees.count(callee)) {
                        if (x_dex.cross_dex_ref(caller, callee)) {
                          candidate_callees.erase(callee);
                          if (callee_caller.count(callee)) {
                            callee_caller.erase(callee);
                          }
                        } else {
                          callee_caller[callee].push_back(caller);
                        }
                      }
                    }
                  });
    for (auto& pair : callee_caller) {
      DexMethod* callee = const_cast<DexMethod*>(pair.first);
      const auto& callers = pair.second;
      for (auto caller : callers) {
        caller_callee[caller].push_back(callee);
      }
    }
  } else {
    walk::opcodes(scope, [](DexMethod* caller) { return true; },
                  [&](DexMethod* caller, IRInstruction* insn) {
                    if (is_invoke(insn->opcode())) {
                      auto callee =
                          resolver(insn->get_method(), opcode_to_search(insn));
                      if (callee != nullptr && callee->is_concrete() &&
                          candidates.count(callee)) {
                        callee_caller[callee].push_back(caller);
                        caller_callee[caller].push_back(callee);
                      }
                    }
                  });
  }
}

void MultiMethodInliner::inline_methods() {
  // we want to inline bottom up, so as a first step we identify all the
  // top level callers, then we recurse into all inlinable callees until we
  // hit a leaf and we start inlining from there
  std::unordered_set<DexMethod*> visited;
  for (auto it : caller_callee) {
    auto caller = it.first;
    TraceContext context(caller->get_deobfuscated_name());
    // if the caller is not a top level keep going, it will be traversed
    // when inlining a top level caller
    if (callee_caller.find(caller) != callee_caller.end()) continue;
    sparta::PatriciaTreeSet<DexMethod*> call_stack;
    caller_inline(caller, it.second, call_stack, &visited);
  }
}

void MultiMethodInliner::caller_inline(
    DexMethod* caller,
    const std::vector<DexMethod*>& callees,
    sparta::PatriciaTreeSet<DexMethod*> call_stack,
    std::unordered_set<DexMethod*>* visited) {
  if (visited->count(caller)) {
    return;
  }
  visited->emplace(caller);
  call_stack.insert(caller);

  std::vector<DexMethod*> nonrecursive_callees;
  nonrecursive_callees.reserve(callees.size());
  // recurse into the callees in case they have something to inline on
  // their own. We want to inline bottom up so that a callee is
  // completely resolved by the time it is inlined.
  for (auto callee : callees) {
    // if the call chain hits a call loop, ignore and keep going
    if (call_stack.contains(callee)) {
      info.recursive++;
      continue;
    }

    auto maybe_caller = caller_callee.find(callee);
    if (maybe_caller != caller_callee.end()) {
      caller_inline(callee, maybe_caller->second, call_stack, visited);
    }

    if (should_inline(caller, callee)) {
      nonrecursive_callees.push_back(callee);
    }
  }
  inline_callees(caller, nonrecursive_callees);
}

void MultiMethodInliner::inline_callees(
    DexMethod* caller, const std::vector<DexMethod*>& callees) {
  size_t found = 0;

  // walk the caller opcodes collecting all candidates to inline
  // Build a callee to opcode map
  std::vector<std::pair<DexMethod*, IRList::iterator>> inlinables;
  editable_cfg_adapter::iterate_with_iterator(
      caller->get_code(), [&](IRList::iterator it) {
        auto insn = it->insn;
        if (!is_invoke(insn->opcode())) {
          return editable_cfg_adapter::LOOP_CONTINUE;
        }
        auto callee = resolver(insn->get_method(), opcode_to_search(insn));
        if (callee == nullptr) {
          return editable_cfg_adapter::LOOP_CONTINUE;
        }
        if (std::find(callees.begin(), callees.end(), callee) ==
            callees.end()) {
          return editable_cfg_adapter::LOOP_CONTINUE;
        }
        always_assert(callee->is_concrete());
        found++;
        inlinables.emplace_back(callee, it);
        if (found == callees.size()) {
          return editable_cfg_adapter::LOOP_BREAK;
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
  if (found != callees.size()) {
    always_assert(found <= callees.size());
    info.not_found += callees.size() - found;
  }

  inline_inlinables(caller, inlinables);
}

void MultiMethodInliner::inline_callees(
    DexMethod* caller, const std::unordered_set<IRInstruction*>& insns) {
  std::vector<std::pair<DexMethod*, IRList::iterator>> inlinables;
  editable_cfg_adapter::iterate_with_iterator(
      caller->get_code(), [&](IRList::iterator it) {
        auto insn = it->insn;
        if (insns.count(insn)) {
          auto callee = resolver(insn->get_method(), opcode_to_search(insn));
          if (callee == nullptr) {
            return editable_cfg_adapter::LOOP_CONTINUE;
          }
          always_assert(callee->is_concrete());
          inlinables.emplace_back(callee, it);
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });

  inline_inlinables(caller, inlinables);
}

void MultiMethodInliner::inline_inlinables(
    DexMethod* caller_method,
    const std::vector<std::pair<DexMethod*, IRList::iterator>>& inlinables) {

  auto caller = caller_method->get_code();
  std::unordered_set<IRCode*> need_deconstruct;
  if (m_config.use_cfg_inliner && !caller->editable_cfg_built()) {
    need_deconstruct.reserve(1 + inlinables.size());
    need_deconstruct.insert(caller);
    for (const auto& inlinable : inlinables) {
      need_deconstruct.insert(inlinable.first->get_code());
    }
    for (auto code : need_deconstruct) {
      always_assert(!code->editable_cfg_built());
      code->build_cfg(/* editable */ true);
    }
  }

  // attempt to inline all inlinable candidates
  size_t estimated_insn_size = caller->editable_cfg_built()
                                   ? caller->cfg().sum_opcode_sizes()
                                   : caller->sum_opcode_sizes();
  for (auto inlinable : inlinables) {
    auto callee_method = inlinable.first;
    auto callee = callee_method->get_code();
    auto callsite = inlinable.second;

    if (!is_inlinable(caller_method, callee_method, callsite->insn,
                      estimated_insn_size)) {
      continue;
    }

    TRACE(MMINL, 4, "inline %s (%d) in %s (%d)\n", SHOW(callee),
          caller->get_registers_size(), SHOW(caller),
          callee->get_registers_size());

    if (m_config.use_cfg_inliner) {
      bool success = inliner::inline_with_cfg(caller_method, callee_method,
                                              callsite->insn);
      if (!success) {
        continue;
      }
    } else {
      // Logging before the call to inline_method to get the most relevant line
      // number near callsite before callsite gets replaced. Should be ok as
      // inline_method does not fail to inline.
      log_opt(INLINED, caller_method, callsite->insn);

      inliner::inline_method(caller, callee, callsite);
    }
    TRACE(INL, 2, "caller: %s\tcallee: %s\n", SHOW(caller), SHOW(callee));
    estimated_insn_size += callee->editable_cfg_built()
                               ? callee->cfg().sum_opcode_sizes()
                               : callee->sum_opcode_sizes();

    TRACE(MMINL, 6, "checking visibility usage of members in %s\n",
          SHOW(callee));
    change_visibility(callee_method);
    info.calls_inlined++;
    inlined.insert(callee_method);
  }

  for (IRCode* code : need_deconstruct) {
    code->clear_cfg();
  }
}

/**
 * Defines the set of rules that determine whether a function is inlinable.
 */
bool MultiMethodInliner::is_inlinable(DexMethod* caller,
                                      DexMethod* callee,
                                      const IRInstruction* insn,
                                      size_t estimated_insn_size) {
  // don't inline cross store references
  if (cross_store_reference(callee)) {
    log_nopt(INL_CROSS_STORE_REFS, caller, insn);
    return false;
  }
  if (is_blacklisted(callee)) {
    log_nopt(INL_BLACKLISTED_CALLEE, callee);
    return false;
  }
  if (caller_is_blacklisted(caller)) {
    log_nopt(INL_BLACKLISTED_CALLER, caller);
    return false;
  }
  if (has_external_catch(callee)) {
    log_nopt(INL_EXTERN_CATCH, callee);
    return false;
  }
  if (cannot_inline_opcodes(caller, callee, insn)) {
    return false;
  }
  if (caller_too_large(caller->get_class(), estimated_insn_size, callee)) {
    log_nopt(INL_TOO_BIG, caller, insn);
    return false;
  }

  // Don't inline code into a method that doesn't have the same (or higher)
  // required API. We don't want to bring API specific code into a class where
  // it's not supported.
  int32_t callee_api = api::LevelChecker::get_method_level(callee);
  if (callee_api != api::LevelChecker::get_min_level() &&
      callee_api > api::LevelChecker::get_method_level(caller)) {
    // check callee_api against the minimum and short-circuit because most
    // methods don't have a required api and we want that to be fast.
    log_nopt(INL_REQUIRES_API, caller, insn);
    TRACE(MMINL, 4,
          "Refusing to inline %s\n"
          "              into %s\n because of API boundaries.",
          show_deobfuscated(callee).c_str(), show_deobfuscated(caller).c_str());
    return false;
  }

  DexClass* cls = type_class(callee->get_class());
  if (has_any_annotation(cls, m_config.no_inline) ||
      has_any_annotation(callee, m_config.no_inline)) {
    return false;
  }

  return true;
}

/**
 * Return whether the method or any of its ancestors are in the blacklist.
 * Typically used to prevent inlining / deletion of methods that are called
 * via reflection.
 */
bool MultiMethodInliner::is_blacklisted(const DexMethod* callee) {
  auto cls = type_class(callee->get_class());
  // Enums are all blacklisted
  if (is_enum(cls)) {
    return true;
  }
  while (cls != nullptr) {
    if (m_config.black_list.count(cls->get_type())) {
      info.blacklisted++;
      return true;
    }
    cls = type_class(cls->get_super_class());
  }
  return false;
}

bool MultiMethodInliner::is_estimate_over_max(uint64_t estimated_caller_size,
                                              const DexMethod* callee,
                                              uint64_t max) {
  // INSTRUCTION_BUFFER is added because the final method size is often larger
  // than our estimate -- during the sync phase, we may have to pick larger
  // branch opcodes to encode large jumps.
  const IRCode* code = callee->get_code();
  auto callee_size = code->editable_cfg_built() ? code->cfg().sum_opcode_sizes()
                                                : code->sum_opcode_sizes();
  if (estimated_caller_size + callee_size > max - INSTRUCTION_BUFFER) {
    info.caller_too_large++;
    return true;
  }
  return false;
}

bool MultiMethodInliner::caller_too_large(DexType* caller_type,
                                          size_t estimated_caller_size,
                                          const DexMethod* callee) {
  if (is_estimate_over_max(estimated_caller_size, callee,
                           HARD_MAX_INSTRUCTION_SIZE)) {
    return true;
  }

  if (!m_config.enforce_method_size_limit) {
    return false;
  }

  if (m_config.whitelist_no_method_limit.count(caller_type)) {
    return false;
  }

  if (is_estimate_over_max(estimated_caller_size, callee,
                           SOFT_MAX_INSTRUCTION_SIZE)) {
    return true;
  }

  return false;
}

bool MultiMethodInliner::should_inline(const DexMethod* caller,
                                       const DexMethod* callee) const {
  if (has_any_annotation(callee, m_config.force_inline)) {
    return true;
  }
  if (too_many_callers(callee)) {
    log_nopt(INL_TOO_MANY_CALLERS, callee);
    return false;
  }
  return true;
}

/*
 * Ignore internal opcodes because they do not take up any space in the final
 * dex file. Ignore move opcodes with the hope that RegAlloc will eliminate
 * most of them.
 */
static size_t count_important_opcodes(const IRCode* code) {
  size_t count{0};
  editable_cfg_adapter::iterate(code, [&](const MethodItemEntry& mie) {
    auto op = mie.insn->opcode();
    if (!opcode::is_internal(op) && !opcode::is_move(op)) {
      ++count;
    }
    return editable_cfg_adapter::LOOP_CONTINUE;
  });
  return count;
}

bool MultiMethodInliner::too_many_callers(const DexMethod* callee) const {
  const auto& callers = callee_caller.at(callee);
  auto caller_count = callers.size();
  always_assert(caller_count > 0);

  if (!m_opcode_counts.count(callee)) {
    m_opcode_counts[callee] = count_important_opcodes(callee->get_code());
  }
  auto code_size = m_opcode_counts.at(callee);

  if (!can_delete(callee)) {
    if (m_config.inline_small_non_deletables) {
      return code_size > CODE_SIZE_ANY_CALLERS;
    } else {
      return true;
    }
  }

  if (caller_count == 1) {
    return false;
  }

  auto callee_class = callee->get_class();
  bool have_all_callers_same_class = true;
  for (auto caller : callers) {
    if (caller->get_class() != callee_class) {
      have_all_callers_same_class = false;
      break;
    }
  }

  unsigned long locality_advantage = have_all_callers_same_class ? 2 : 0;
  if (m_config.multiple_callers) {
    switch (caller_count) {
    case 2:
      return code_size > CODE_SIZE_2_CALLERS + locality_advantage;
    case 3:
      return code_size > CODE_SIZE_3_CALLERS + locality_advantage;
    default:
      break;
    }
  }
  return code_size > CODE_SIZE_ANY_CALLERS + locality_advantage;
}

bool MultiMethodInliner::caller_is_blacklisted(const DexMethod* caller) {
  auto cls = caller->get_class();
  if (m_config.caller_black_list.count(cls)) {
    info.blacklisted++;
    return true;
  }
  return false;
}

/**
 * Returns true if the callee has catch type which is external and not public,
 * in which case we cannot inline.
 */
bool MultiMethodInliner::has_external_catch(const DexMethod* callee) {
  const IRCode* code = callee->get_code();
  std::vector<DexType*> types;
  if (code->editable_cfg_built()) {
    code->cfg().gather_catch_types(types);
  } else {
    code->gather_catch_types(types);
  }
  for (auto type : types) {
    auto cls = type_class(type);
    if (cls != nullptr && cls->is_external() && !is_public(cls)) {
      return true;
    }
  }
  return false;
}

/**
 * Analyze opcodes in the callee to see if they are problematic for inlining.
 */
bool MultiMethodInliner::cannot_inline_opcodes(const DexMethod* caller,
                                               const DexMethod* callee,
                                               const IRInstruction* invk_insn) {
  int ret_count = 0;
  bool can_inline = true;
  editable_cfg_adapter::iterate(
      callee->get_code(), [&](const MethodItemEntry& mie) {
        auto insn = mie.insn;
        if (create_vmethod(insn)) {
          log_nopt(INL_CREATE_VMETH, caller, invk_insn);
          can_inline = false;
          return editable_cfg_adapter::LOOP_BREAK;
        }
        if (nonrelocatable_invoke_super(insn, callee, caller)) {
          log_nopt(INL_HAS_INVOKE_SUPER, caller, invk_insn);
          can_inline = false;
          return editable_cfg_adapter::LOOP_BREAK;
        }
        // if the caller and callee are in the same class, we don't have to
        // worry about unknown virtuals -- private / protected methods will
        // remain accessible
        if (caller->get_class() != callee->get_class()) {
          if (unknown_virtual(insn)) {
            log_nopt(INL_UNKNOWN_VIRTUAL, caller, invk_insn);
            can_inline = false;
            return editable_cfg_adapter::LOOP_BREAK;
          }
          if (unknown_field(insn)) {
            log_nopt(INL_UNKNOWN_FIELD, caller, invk_insn);
            can_inline = false;
            return editable_cfg_adapter::LOOP_BREAK;
          }
          if (check_android_os_version(insn)) {
            can_inline = false;
            return editable_cfg_adapter::LOOP_BREAK;
          }
        }
        if (!m_config.throws_inline && insn->opcode() == OPCODE_THROW) {
          info.throws++;
          can_inline = false;
          return editable_cfg_adapter::LOOP_BREAK;
        }
        if (is_return(insn->opcode())) {
          ++ret_count;
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
  // The IRCode inliner can't handle callees with more than one return
  // statement (normally one, the way dx generates code). That allows us to make
  // a simple inline strategy where we don't have to worry about creating
  // branches from the multiple returns to the main code
  //
  // d8 however, generates code with multiple return statements in general.
  // The CFG inliner can handle multiple return callees.
  if (ret_count > 1 && !m_config.use_cfg_inliner) {
    info.multi_ret++;
    log_nopt(INL_MULTIPLE_RETURNS, callee);
    can_inline = false;
  }
  return !can_inline;
}

/**
 * Check if a visibility/accessibility change would turn a method referenced
 * in a callee to virtual methods as they are inlined into the caller.
 * That is, once a callee is inlined we need to ensure that everything that was
 * referenced by a callee is visible and accessible in the caller context.
 * This step would not be needed if we changed all private instance to static.
 */
bool MultiMethodInliner::create_vmethod(IRInstruction* insn) {
  auto opcode = insn->opcode();
  if (opcode == OPCODE_INVOKE_DIRECT) {
    auto method = resolver(insn->get_method(), MethodSearch::Direct);
    if (method == nullptr) {
      info.need_vmethod++;
      return true;
    }
    always_assert(method->is_def());
    if (is_init(method)) {
      if (!method->is_concrete() && !is_public(method)) {
        info.non_pub_ctor++;
        return true;
      }
      // concrete ctors we can handle because they stay invoke_direct
      return false;
    }
    if (!is_native(method) && !has_keep(method)) {
      m_make_static.insert(method);
    } else {
      info.need_vmethod++;
      return true;
    }
  }
  return false;
}

/**
 * Return true if a callee contains an invoke super to a different method
 * in the hierarchy, and the callee and caller are in different classes.
 * Inlining an invoke_super off its class hierarchy would break the verifier.
 */
bool MultiMethodInliner::nonrelocatable_invoke_super(IRInstruction* insn,
                                                     const DexMethod* callee,
                                                     const DexMethod* caller) {
  if (insn->opcode() == OPCODE_INVOKE_SUPER) {
    if (callee->get_class() == caller->get_class()) {
      return false;
    }
    info.invoke_super++;
    return true;
  }
  return false;
}

/**
 * The callee contains an invoke to a virtual method we either do not know
 * or it's not public. Given the caller may not be in the same
 * hierarchy/package we cannot inline it unless we make the method public.
 * But we need to make all methods public across the hierarchy and for methods
 * we don't know we have no idea whether the method was public or not anyway.
 */
bool MultiMethodInliner::unknown_virtual(IRInstruction* insn) {
  if (insn->opcode() == OPCODE_INVOKE_VIRTUAL) {
    auto method = insn->get_method();
    auto res_method = resolver(method, MethodSearch::Virtual);
    if (res_method == nullptr) {
      // if it's not known to redex but it's a common java/android API method
      if (method_ok(method->get_class(), method)) {
        return false;
      }
      auto type = method->get_class();
      if (type_ok(type)) return false;

      // the method ref is bound to a type known to redex but the method does
      // not exist in the hierarchy known to redex. Essentially the method
      // is from an external type i.e. A.equals(Object)
      auto cls = type_class(type);
      while (cls != nullptr) {
        type = cls->get_super_class();
        cls = type_class(type);
      }
      if (type_ok(type)) return false;
      if (method_ok(type, method)) return false;
      info.escaped_virtual++;
      return true;
    }
    if (res_method->is_external() && !is_public(res_method)) {
      info.non_pub_virtual++;
      return true;
    }
  }
  return false;
}

/**
 * The callee contains a *get/put instruction to an unknown field.
 * Given the caller may not be in the same hierarchy/package we cannot inline
 * it unless we make the field public.
 * But we need to make all fields public across the hierarchy and for fields
 * we don't know we have no idea whether the field was public or not anyway.
 */
bool MultiMethodInliner::unknown_field(IRInstruction* insn) {
  if (is_ifield_op(insn->opcode()) || is_sfield_op(insn->opcode())) {
    auto ref = insn->get_field();
    DexField* field = resolve_field(ref, is_sfield_op(insn->opcode())
        ? FieldSearch::Static : FieldSearch::Instance);
    if (field == nullptr) {
      info.escaped_field++;
      return true;
    }
    if (!field->is_concrete() && !is_public(field)) {
      info.non_pub_field++;
      return true;
    }
  }
  return false;
}

/**
 * return true if `insn` is
 *   sget android.os.Build.VERSION.SDK_INT
 */
bool MultiMethodInliner::check_android_os_version(IRInstruction* insn) {
  // Referencing a method or field that doesn't exist on the OS version of the
  // current device causes a "soft error" for the entire class that the
  // reference resides in. Soft errors aren't worrisome from a correctness
  // perspective (though they may cause the class to run slower on some devices)
  // but there's a bug in Android 5 that triggers an erroneous "hard error"
  // after a "soft error".
  //
  // The exact conditions that trigger the Android 5 bug aren't currently known.
  // As a quick fix, we're refusing to inline methods that check the OS's
  // version. This generally works because the reference to the non-existent
  // field/method is usually guarded by checking that
  // `android.os.build.VERSION.SDK_INT` is larger than the required api level.
  auto op = insn->opcode();
  if (is_sget(op)) {
    auto ref = insn->get_field();
    DexField* field = resolve_field(ref, FieldSearch::Static);
    if (field != nullptr &&
        field == DexField::get_field("Landroid/os/Build$VERSION;.SDK_INT:I")) {
      return true;
    }
  }
  return false;
}

bool MultiMethodInliner::cross_store_reference(const DexMethod* callee) {
  size_t store_idx = xstores.get_store_idx(callee->get_class());
  bool has_cross_store_ref = false;
  editable_cfg_adapter::iterate(
      callee->get_code(), [&](const MethodItemEntry& mie) {
        auto insn = mie.insn;
        if (insn->has_type()) {
          if (xstores.illegal_ref(store_idx, insn->get_type())) {
            info.cross_store++;
            has_cross_store_ref = true;
            return editable_cfg_adapter::LOOP_BREAK;
          }
        } else if (insn->has_method()) {
          auto meth = insn->get_method();
          if (xstores.illegal_ref(store_idx, meth->get_class())) {
            info.cross_store++;
            has_cross_store_ref = true;
            return editable_cfg_adapter::LOOP_BREAK;
          }
          auto proto = meth->get_proto();
          if (xstores.illegal_ref(store_idx, proto->get_rtype())) {
            info.cross_store++;
            has_cross_store_ref = true;
            return editable_cfg_adapter::LOOP_BREAK;
          }
          auto args = proto->get_args();
          if (args == nullptr) {
            return editable_cfg_adapter::LOOP_CONTINUE;
          }
          for (const auto& arg : args->get_type_list()) {
            if (xstores.illegal_ref(store_idx, arg)) {
              info.cross_store++;
              has_cross_store_ref = true;
              return editable_cfg_adapter::LOOP_BREAK;
            }
          }
        } else if (insn->has_field()) {
          auto field = insn->get_field();
          if (xstores.illegal_ref(store_idx, field->get_class()) ||
              xstores.illegal_ref(store_idx, field->get_type())) {
            info.cross_store++;
            has_cross_store_ref = true;
            return editable_cfg_adapter::LOOP_BREAK;
          }
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
  return has_cross_store_ref;
}

void MultiMethodInliner::invoke_direct_to_static() {
  // We sort the methods here because make_static renames methods on collision,
  // and which collisions occur is order-dependent. E.g. if we have the
  // following methods in m_make_static:
  //
  //   Foo Foo::bar()
  //   Foo Foo::bar(Foo f)
  //
  // making Foo::bar() static first would make it collide with Foo::bar(Foo f),
  // causing it to get renamed to bar$redex0(). But if Foo::bar(Foo f) gets
  // static-ified first, it becomes Foo::bar(Foo f, Foo f), so when bar() gets
  // made static later there is no collision. So in the interest of having
  // reproducible binaries, we sort the methods first.
  //
  // Also, we didn't use an std::set keyed by method signature here because
  // make_static is mutating the signatures. The tree that implements the set
  // would have to be rebalanced after the mutations.
  std::vector<DexMethod*> methods(m_make_static.begin(), m_make_static.end());
  std::sort(methods.begin(), methods.end(), compare_dexmethods);
  for (auto method : methods) {
    TRACE(MMINL, 6, "making %s static\n", method->get_name()->c_str());
    mutators::make_static(method);
  }
  walk::opcodes(m_scope, [](DexMethod* meth) { return true; },
      [&](DexMethod*, IRInstruction* insn) {
        auto op = insn->opcode();
        if (op == OPCODE_INVOKE_DIRECT) {
          if (m_make_static.count(
              static_cast<DexMethod*>(insn->get_method()))) {
            insn->set_opcode(OPCODE_INVOKE_STATIC);
          }
        }
      });
}

void adjust_opcode_counts(
    const std::unordered_multimap<DexMethod*, DexMethod*>& callee_to_callers,
    DexMethod* callee,
    std::unordered_map<DexMethod*, size_t>* adjusted_opcode_count) {
  auto code = callee->get_code();
  if (code == nullptr) {
    return;
  }
  auto code_size = code->count_opcodes();
  auto range = callee_to_callers.equal_range(callee);
  for (auto it = range.first; it != range.second; ++it) {
    auto caller = it->second;
    (*adjusted_opcode_count)[caller] += code_size;
  }
}

namespace {

using RegMap = transform::RegMap;

/*
 * Expands the caller register file by the size of the callee register file,
 * and allocates the high registers to the callee. E.g. if we have a caller
 * with registers_size of M and a callee with registers_size N, this function
 * will resize the caller's register file to M + N and map register k in the
 * callee to M + k in the caller. It also inserts move instructions to map the
 * callee arguments to the newly allocated registers.
 */
std::unique_ptr<RegMap> gen_callee_reg_map(
    IRCode* caller_code,
    const IRCode* callee_code,
    IRList::iterator invoke_it) {
  auto callee_reg_start = caller_code->get_registers_size();
  auto insn = invoke_it->insn;
  auto reg_map = std::make_unique<RegMap>();

  // generate the callee register map
  for (auto i = 0; i < callee_code->get_registers_size(); ++i) {
    reg_map->emplace(i, callee_reg_start + i);
  }

  // generate and insert the move instructions
  auto param_insns =
      InstructionIterable(callee_code->get_param_instructions());
  auto param_it = param_insns.begin();
  auto param_end = param_insns.end();
  for (size_t i = 0; i < insn->srcs_size(); ++i, ++param_it) {
    always_assert(param_it != param_end);
    auto mov =
        (new IRInstruction(opcode::load_param_to_move(param_it->insn->opcode())))
            ->set_src(0, insn->src(i))
            ->set_dest(callee_reg_start + param_it->insn->dest());
    caller_code->insert_before(invoke_it, mov);
  }
  caller_code->set_registers_size(callee_reg_start +
                                  callee_code->get_registers_size());
  return reg_map;
}

/**
 * Create a move instruction given a return instruction in a callee and
 * a move-result instruction in a caller.
 */
IRInstruction* move_result(IRInstruction* res, IRInstruction* move_res) {
  auto opcode = res->opcode();
  always_assert(opcode != OPCODE_RETURN_VOID);
  IRInstruction* move;
  if (opcode == OPCODE_RETURN_OBJECT) {
    move = new IRInstruction(OPCODE_MOVE_OBJECT);
  } else if (opcode == OPCODE_RETURN_WIDE) {
    move = new IRInstruction(OPCODE_MOVE_WIDE);
  } else {
    always_assert(opcode == OPCODE_RETURN);
    move = new IRInstruction(OPCODE_MOVE);
  }
  move->set_dest(move_res->dest());
  move->set_src(0, res->src(0));
  return move;
}

/*
 * Map the callee's param registers to the argument registers of the caller.
 * Any other callee register N will get mapped to caller_registers_size + N.
 * The resulting callee code can then be appended to the caller's code without
 * any register conflicts.
 */
void remap_callee_for_tail_call(const IRCode* caller_code,
                                IRCode* callee_code,
                                IRList::iterator invoke_it) {
  RegMap reg_map;
  auto insn = invoke_it->insn;
  auto callee_reg_start = caller_code->get_registers_size();

  auto param_insns =
      InstructionIterable(callee_code->get_param_instructions());
  auto param_it = param_insns.begin();
  auto param_end = param_insns.end();
  for (size_t i = 0; i < insn->srcs_size(); ++i, ++param_it) {
    always_assert_log(
        param_it != param_end, "no param insns\n%s", SHOW(callee_code));
    reg_map[param_it->insn->dest()] = insn->src(i);
  }
  for (size_t i = 0; i < callee_code->get_registers_size(); ++i) {
    if (reg_map.count(i) != 0) {
      continue;
    }
    reg_map[i] = callee_reg_start + i;
  }
  transform::remap_registers(callee_code, reg_map);
}

void cleanup_callee_debug(IRCode* callee_code) {
  std::unordered_set<uint16_t> valid_regs;
  auto it = callee_code->begin();
  while (it != callee_code->end()) {
    auto& mei = *it++;
    if (mei.type == MFLOW_DEBUG) {
      switch(mei.dbgop->opcode()) {
      case DBG_SET_PROLOGUE_END:
        callee_code->erase(callee_code->iterator_to(mei));
        break;
      case DBG_START_LOCAL:
      case DBG_START_LOCAL_EXTENDED: {
        auto reg = mei.dbgop->uvalue();
        valid_regs.insert(reg);
        break;
      }
      case DBG_END_LOCAL:
      case DBG_RESTART_LOCAL: {
        auto reg = mei.dbgop->uvalue();
        if (valid_regs.find(reg) == valid_regs.end()) {
          callee_code->erase(callee_code->iterator_to(mei));
        }
        break;
      }
      default:
        break;
      }
    }
  }
}
} // namespace

/*
 * For splicing a callee's IRList into a caller.
 */
class MethodSplicer {
  IRCode* m_mtcaller;
  IRCode* m_mtcallee;
  MethodItemEntryCloner m_mie_cloner;
  const RegMap& m_callee_reg_map;
  DexPosition* m_invoke_position;
  MethodItemEntry* m_active_catch;
  std::unordered_set<uint16_t> m_valid_dbg_regs;

 public:
  MethodSplicer(IRCode* mtcaller,
                IRCode* mtcallee,
                const RegMap& callee_reg_map,
                DexPosition* invoke_position,
                MethodItemEntry* active_catch)
      : m_mtcaller(mtcaller),
        m_mtcallee(mtcallee),
        m_callee_reg_map(callee_reg_map),
        m_invoke_position(invoke_position),
        m_active_catch(active_catch) {
  }

  MethodItemEntry* clone(MethodItemEntry* mie) {
    auto result = m_mie_cloner.clone(mie);
    return result;
  }

  void operator()(IRList::iterator insert_pos,
                  IRList::iterator fcallee_start,
                  IRList::iterator fcallee_end) {
    std::vector<DexPosition*> positions_to_fix;
    for (auto it = fcallee_start; it != fcallee_end; ++it) {
      if (should_skip_debug(&*it)) {
        continue;
      }
      if (it->type == MFLOW_OPCODE &&
          opcode::is_load_param(it->insn->opcode())) {
        continue;
      }
      auto mie = clone(&*it);
      transform::remap_registers(*mie, m_callee_reg_map);
      if (mie->type == MFLOW_TRY && m_active_catch != nullptr) {
        auto tentry = mie->tentry;
        // try ranges cannot be nested, so we flatten them here
        switch (tentry->type) {
          case TRY_START:
            m_mtcaller->insert_before(insert_pos,
                *(new MethodItemEntry(TRY_END, m_active_catch)));
            m_mtcaller->insert_before(insert_pos, *mie);
            break;
          case TRY_END:
            m_mtcaller->insert_before(insert_pos, *mie);
            m_mtcaller->insert_before(insert_pos,
                *(new MethodItemEntry(TRY_START, m_active_catch)));
            break;
        }
      } else {
        if (mie->type == MFLOW_POSITION && mie->pos->parent == nullptr) {
          mie->pos->parent = m_invoke_position;
        }
        // if a handler list does not terminate in a catch-all, have it point to
        // the parent's active catch handler. TODO: Make this more precise by
        // checking if the parent catch type is a subtype of the callee's.
        if (mie->type == MFLOW_CATCH && mie->centry->next == nullptr &&
            mie->centry->catch_type != nullptr) {
          mie->centry->next = m_active_catch;
        }
        m_mtcaller->insert_before(insert_pos, *mie);
      }
    }
  }

  void fix_parent_positions() {
    m_mie_cloner.fix_parent_positions(m_invoke_position);
  }

 private:
  /* We need to skip two cases:
   * Duplicate DBG_SET_PROLOGUE_END
   * Uninitialized parameters
   *
   * The parameter names are part of the debug info for the method.
   * The technically correct solution would be to make a start
   * local for each of them.  However, that would also imply another
   * end local after the tail to correctly set what the register
   * is at the end.  This would bloat the debug info parameters for
   * a corner case.
   *
   * Instead, we just delete locals lifetime information for parameters.
   * This is an exceedingly rare case triggered by goofy code that
   * reuses parameters as locals.
   */
  bool should_skip_debug(const MethodItemEntry* mei) {
    if (mei->type != MFLOW_DEBUG) {
      return false;
    }
    switch (mei->dbgop->opcode()) {
    case DBG_SET_PROLOGUE_END:
      return true;
    case DBG_START_LOCAL:
    case DBG_START_LOCAL_EXTENDED: {
      auto reg = mei->dbgop->uvalue();
      m_valid_dbg_regs.insert(reg);
      return false;
    }
    case DBG_END_LOCAL:
    case DBG_RESTART_LOCAL: {
      auto reg = mei->dbgop->uvalue();
      if (m_valid_dbg_regs.find(reg) == m_valid_dbg_regs.end()) {
        return true;
      }
    }
    default:
      return false;
    }
  }
};

namespace inliner {

DexPosition* last_position_before(const IRList::const_iterator& it,
                                  const IRCode* code) {
  // we need to decrement the reverse iterator because it gets constructed
  // as pointing to the element preceding pos
  auto position_it = std::prev(IRList::const_reverse_iterator(it));
  const auto& rend = code->rend();
  while (++position_it != rend && position_it->type != MFLOW_POSITION)
    ;
  return position_it == rend ? nullptr : position_it->pos.get();
}

void inline_method(IRCode* caller_code,
                   IRCode* callee_code,
                   IRList::iterator pos) {
  TRACE(INL, 5, "caller code:\n%s\n", SHOW(caller_code));
  TRACE(INL, 5, "callee code:\n%s\n", SHOW(callee_code));

  auto callee_reg_map = gen_callee_reg_map(caller_code, callee_code, pos);

  // find the move-result after the invoke, if any. Must be the first
  // instruction after the invoke
  auto move_res = pos;
  while (move_res++ != caller_code->end() && move_res->type != MFLOW_OPCODE);
  if (!is_move_result(move_res->insn->opcode())) {
    move_res = caller_code->end();
  }

  // find the last position entry before the invoke.
  const auto invoke_position = last_position_before(pos, caller_code);
  if (invoke_position) {
    TRACE(INL, 3, "Inlining call at %s:%d\n",
          invoke_position->file->c_str(),
          invoke_position->line);
  }

  // check if we are in a try block
  auto caller_catch = transform::find_active_catch(caller_code, pos);

  const auto& ret_it = std::find_if(
      callee_code->begin(), callee_code->end(), [](const MethodItemEntry& mei) {
        return mei.type == MFLOW_OPCODE && is_return(mei.insn->opcode());
      });

  auto splice = MethodSplicer(caller_code,
                              callee_code,
                              *callee_reg_map,
                              invoke_position,
                              caller_catch);
  // Copy the callee up to the return. Everything else we push at the end
  // of the caller
  splice(pos, callee_code->begin(), ret_it);

  // try items can span across a return opcode
  auto callee_catch =
      splice.clone(transform::find_active_catch(callee_code, ret_it));
  if (callee_catch != nullptr) {
    caller_code->insert_before(pos,
                               *(new MethodItemEntry(TRY_END, callee_catch)));
    if (caller_catch != nullptr) {
      caller_code->insert_before(
          pos, *(new MethodItemEntry(TRY_START, caller_catch)));
    }
  }

  if (move_res != caller_code->end() && ret_it != callee_code->end()) {
    auto ret_insn = std::make_unique<IRInstruction>(*ret_it->insn);
    transform::remap_registers(ret_insn.get(), *callee_reg_map);
    IRInstruction* move = move_result(ret_insn.get(), move_res->insn);
    auto move_mei = new MethodItemEntry(move);
    caller_code->insert_before(pos, *move_mei);
  }
  // ensure that the caller's code after the inlined method retain their
  // original position
  if (invoke_position) {
    caller_code->insert_before(pos,
                    *(new MethodItemEntry(
                        std::make_unique<DexPosition>(*invoke_position))));
  }

  // remove invoke
  caller_code->erase_and_dispose(pos);
  // remove move_result
  if (move_res != caller_code->end()) {
    caller_code->erase_and_dispose(move_res);
  }

  if (ret_it != callee_code->end()) {
    if (callee_catch != nullptr) {
      caller_code->push_back(*(new MethodItemEntry(TRY_START, callee_catch)));
    } else if (caller_catch != nullptr) {
      caller_code->push_back(*(new MethodItemEntry(TRY_START, caller_catch)));
    }

    if (std::next(ret_it) != callee_code->end()) {
      const auto return_position = last_position_before(ret_it, callee_code);
      if (return_position) {
        // If there are any opcodes between the callee's return and its next
        // position, we need to re-mark them with the correct line number,
        // otherwise they would inherit the line number from the end of the
        // caller.
        auto new_pos = std::make_unique<DexPosition>(*return_position);
        // We want its parent to be the same parent as other inlined code.
        new_pos->parent = invoke_position;
        caller_code->push_back(*(new MethodItemEntry(std::move(new_pos))));
      }
    }

    // Copy the opcodes in the callee after the return and put them at the end
    // of the caller.
    splice(caller_code->end(), std::next(ret_it), callee_code->end());
    if (caller_catch != nullptr) {
      caller_code->push_back(*(new MethodItemEntry(TRY_END, caller_catch)));
    }
  }
  splice.fix_parent_positions();
  TRACE(INL, 5, "post-inline caller code:\n%s\n", SHOW(caller_code));
}

void inline_tail_call(DexMethod* caller,
                      DexMethod* callee,
                      IRList::iterator pos) {
  TRACE(INL, 2, "caller: %s\ncallee: %s\n", SHOW(caller), SHOW(callee));
  auto* caller_code = caller->get_code();
  auto* callee_code = callee->get_code();

  remap_callee_for_tail_call(caller_code, callee_code, pos);
  caller_code->set_registers_size(caller_code->get_registers_size() +
                                  callee_code->get_registers_size());

  cleanup_callee_debug(callee_code);
  auto it = callee_code->begin();
  while (it != callee_code->end()) {
    auto& mei = *it++;
    if (mei.type == MFLOW_OPCODE && opcode::is_load_param(mei.insn->opcode())) {
      continue;
    }
    callee_code->erase(callee_code->iterator_to(mei));
    caller_code->insert_before(pos, mei);
  }
  // Delete the vestigial tail.
  while (pos != caller_code->end()) {
    if (pos->type == MFLOW_OPCODE) {
      pos = caller_code->erase_and_dispose(pos);
    } else {
      ++pos;
    }
  }
}

// return true on successful inlining, false otherwise
bool inline_with_cfg(DexMethod* caller_method,
                     DexMethod* callee_method,
                     IRInstruction* callsite) {

  auto caller_code = caller_method->get_code();
  always_assert(caller_code->editable_cfg_built());
  auto& caller_cfg = caller_code->cfg();
  const cfg::InstructionIterator& callsite_it = caller_cfg.find_insn(callsite);
  if (callsite_it.is_end()) {
    // The callsite is not in the caller cfg. This is probably because the
    // callsite pointer is stale. Maybe the callsite's block was deleted since
    // the time the callsite was found.
    //
    // This could have happened if a previous inlining caused a block to be
    // unreachable, and that block was deleted when the CFG was simplified.
    return false;
  }

  // Logging before the call to inline_cfg to get the most relevant line
  // number near callsite before callsite gets replaced. Should be ok as
  // inline_cfg does not fail to inline.
  log_opt(INLINED, caller_method, callsite);

  auto callee_code = callee_method->get_code();
  always_assert(callee_code->editable_cfg_built());
  cfg::CFGInliner::inline_cfg(&caller_cfg, callsite_it, callee_code->cfg());

  return true;
}

} // namespace inliner
