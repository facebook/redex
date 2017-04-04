/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "InlineHelper.h"
#include "IRInstruction.h"
#include "DexUtil.h"
#include "Mutators.h"
#include "Resolver.h"
#include "Walkers.h"

namespace {

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

constexpr int MAX_INSTRUCTION_SIZE = 1 << 16;
constexpr int INSTRUCTION_BUFFER = 1 << 12;

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
bool method_ok(DexType* type, DexMethod* meth) {
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

}

MultiMethodInliner::MultiMethodInliner(
    const std::vector<DexClass*>& scope,
    const DexClasses& primary_dex,
    const std::unordered_set<DexMethod*>& candidates,
    std::function<DexMethod*(DexMethod*, MethodSearch)> resolver,
    const Config& config)
    : resolver(resolver), m_scope(scope), m_config(config) {
  for (const auto& cls : primary_dex) {
    primary.insert(cls->get_type());
  }
  // walk every opcode in scope looking for calls to inlinable candidates
  // and build a map of callers to callees and the reverse callees to callers
  walk_opcodes(scope, [](DexMethod* meth) { return true; },
      [&](DexMethod* meth, IRInstruction* opcode) {
        if (is_invoke(opcode->opcode())) {
          auto mop = static_cast<IRMethodInstruction*>(opcode);
          auto callee = resolver(mop->get_method(), opcode_to_search(opcode));
          if (callee != nullptr && callee->is_concrete() &&
              candidates.find(callee) != candidates.end()) {
            callee_caller[callee].push_back(meth);
            caller_callee[meth].push_back(callee);
          }
        }
      });
}

void MultiMethodInliner::inline_methods() {
  // we want to inline bottom up, so as a first step we identify all the
  // top level callers, then we recurse into all inlinable callees until we
  // hit a leaf and we start inlining from there
  for (auto it : caller_callee) {
    auto caller = it.first;
    // if the caller is not a top level keep going, it will be traversed
    // when inlining a top level caller
    if (callee_caller.find(caller) != callee_caller.end()) continue;
    std::unordered_set<DexMethod*> visited;
    visited.insert(caller);
    caller_inline(caller, it.second, visited);
  }

  invoke_direct_to_static();
}

void MultiMethodInliner::caller_inline(
    DexMethod* caller,
    const std::vector<DexMethod*>& callees,
    std::unordered_set<DexMethod*>& visited) {
  std::vector<DexMethod*> nonrecursive_callees;
  nonrecursive_callees.reserve(callees.size());
  // recurse into the callees in case they have something to inline on
  // their own. We want to inline bottom up so that a callee is
  // completely resolved by the time it is inlined.
  for (auto callee : callees) {
    // if the call chain hits a call loop, ignore and keep going
    if (visited.count(callee) > 0) {
      info.recursive++;
      continue;
    }
    nonrecursive_callees.push_back(callee);

    auto maybe_caller = caller_callee.find(callee);
    if (maybe_caller != caller_callee.end()) {
      visited.insert(callee);
      caller_inline(callee, maybe_caller->second, visited);
      visited.erase(callee);
    }
  }
  inline_callees(caller, nonrecursive_callees);
}

void MultiMethodInliner::inline_callees(
    DexMethod* caller, const std::vector<DexMethod*>& callees) {
  size_t found = 0;

  // walk the caller opcodes collecting all candidates to inline
  // Build a callee to opcode map
  std::vector<std::pair<DexMethod*, IRMethodInstruction*>> inlinables;
  for (auto& mie : InstructionIterable(caller->get_code())) {
    auto insn = mie.insn;
    if (!is_invoke(insn->opcode())) continue;
    auto mop = static_cast<IRMethodInstruction*>(insn);
    auto callee = resolver(mop->get_method(), opcode_to_search(insn));
    if (callee == nullptr) continue;
    if (std::find(callees.begin(), callees.end(), callee) == callees.end()) {
      continue;
    }
    always_assert(callee->is_concrete());
    found++;
    inlinables.push_back(std::make_pair(callee, mop));
    if (found == callees.size()) break;
  }
  if (found != callees.size()) {
    always_assert(found <= callees.size());
    info.not_found += callees.size() - found;
  }

  // attempt to inline all inlinable candidates
  InlineContext inline_context(caller, m_config.use_liveness);
  for (auto inlinable : inlinables) {
    auto callee = inlinable.first;
    auto mop = inlinable.second;

    if (!is_inlinable(inline_context, callee, caller)) continue;

    TRACE(MMINL, 4, "inline %s (%d) in %s (%d)\n",
        SHOW(callee), caller->get_code()->get_registers_size(),
        SHOW(caller),
        callee->get_code()->get_registers_size() -
        callee->get_code()->get_ins_size());
    if (!IRCode::inline_method(
            inline_context, callee, mop, m_config.no_exceed_16regs)) {
      info.more_than_16regs++;
      continue;
    }
    TRACE(INL, 2, "caller: %s\tcallee: %s\n", SHOW(caller), SHOW(callee));
    inline_context.estimated_insn_size +=
        callee->get_code()->sum_opcode_sizes();
    change_visibility(callee);
    info.calls_inlined++;
    inlined.insert(callee);
  }
}

/**
 * Defines the set of rules that determine whether a function is inlinable.
 */
bool MultiMethodInliner::is_inlinable(InlineContext& ctx,
                                      DexMethod* callee,
                                      DexMethod* caller) {
  // don't bring anything into primary that is not in primary
  if (primary.count(caller->get_class()) != 0 && refs_not_in_primary(callee)) {
    return false;
  }
  if (is_blacklisted(callee)) return false;
  if (caller_is_blacklisted(caller)) return false;
  if (has_external_catch(callee)) return false;
  if (cannot_inline_opcodes(callee, caller)) return false;
  if (caller_too_large(ctx, callee)) return false;

  return true;
}

/**
 * Return whether the method or any of its ancestors are in the blacklist.
 * Typically used to prevent inlining / deletion of methods that are called
 * via reflection.
 */
bool MultiMethodInliner::is_blacklisted(DexMethod* callee) {
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

bool MultiMethodInliner::caller_too_large(InlineContext& ctx,
                                          DexMethod* callee) {
  // INSTRUCTION_BUFFER is added because the final method size is often larger
  // than our estimate -- during the sync phase, we may have to pick larger
  // branch opcodes to encode large jumps.
  auto insns_size = callee->get_code()->sum_opcode_sizes();
  if (ctx.estimated_insn_size + insns_size >
      MAX_INSTRUCTION_SIZE - INSTRUCTION_BUFFER) {
    info.caller_too_large++;
    return true;
  }
  return false;
}

bool MultiMethodInliner::caller_is_blacklisted(DexMethod* caller) {
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
bool MultiMethodInliner::has_external_catch(DexMethod* callee) {
  std::vector<DexType*> types;
  callee->get_code()->gather_catch_types(types);
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
bool MultiMethodInliner::cannot_inline_opcodes(DexMethod* callee,
                                               DexMethod* caller) {
  int ret_count = 0;
  for (auto& mie : InstructionIterable(callee->get_code())) {
    auto insn = mie.insn;
    if (create_vmethod(insn)) return true;
    if (nonrelocatable_invoke_super(insn, callee, caller)) return true;
    if (unknown_virtual(insn, callee, caller)) return true;
    if (unknown_field(insn, callee, caller)) return true;
    if (!m_config.throws_inline && insn->opcode() == OPCODE_THROW) {
      info.throws++;
      return true;
    }
    if (is_return(insn->opcode())) ret_count++;
  }
  // no callees that have more than a return statement (normally one, the
  // way dx generates code).
  // That allows us to make a simple inline strategy where we don't have to
  // worry about creating branches from the multiple returns to the main code
  if (ret_count > 1) {
    info.multi_ret++;
    return true;
  }
  return false;
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
  if (opcode == OPCODE_INVOKE_DIRECT || opcode == OPCODE_INVOKE_DIRECT_RANGE) {
    auto method = static_cast<IRMethodInstruction*>(insn)->get_method();
    method = resolver(method, MethodSearch::Direct);
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
    if (m_config.callee_direct_invoke_inline &&
        !is_native(method)) {
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
                                                     DexMethod* callee,
                                                     DexMethod* caller) {
  if (insn->opcode() == OPCODE_INVOKE_SUPER ||
      insn->opcode() == OPCODE_INVOKE_SUPER_RANGE) {
    if (m_config.super_same_class_inline &&
        callee->get_class() == caller->get_class()) {
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

bool MultiMethodInliner::unknown_virtual(IRInstruction* insn,
                                         DexMethod* callee,
                                         DexMethod* caller) {
  // if the caller and callee are in the same class, we don't have to worry
  // about unknown virtuals -- private / protected methods will remain
  // accessible
  if (m_config.virtual_same_class_inline &&
      caller->get_class() == callee->get_class()) {
    return false;
  }
  if (insn->opcode() == OPCODE_INVOKE_VIRTUAL ||
      insn->opcode() == OPCODE_INVOKE_VIRTUAL_RANGE) {
    auto method = static_cast<IRMethodInstruction*>(insn)->get_method();
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
bool MultiMethodInliner::unknown_field(IRInstruction* insn,
                                       DexMethod* callee,
                                       DexMethod* caller) {
  // if the caller and callee are in the same class, we don't have to worry
  // about unknown fields -- private / protected fields will remain
  // accessible
  if (m_config.virtual_same_class_inline &&
      caller->get_class() == callee->get_class()) {
    return false;
  }
  if (is_ifield_op(insn->opcode()) || is_sfield_op(insn->opcode())) {
    auto fop = static_cast<IRFieldInstruction*>(insn);
    auto field = fop->field();
    field = resolve_field(field, is_sfield_op(insn->opcode())
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
 * If the caller is in the primary DEX we want to make sure there are no
 * references in other DEXes that may cause a verification error.
 * Don't inline if so.
 */
bool MultiMethodInliner::refs_not_in_primary(DexMethod* callee) {

  const auto ok_from_primary = [&](DexType* type) {
    if (primary.count(type) == 0 && type_class_internal(type) != nullptr) {
      info.not_in_primary++;
      return false;
    }
    return true;
  };

  for (auto& mie : InstructionIterable(callee->get_code())) {
    auto insn = mie.insn;
    if (insn->has_types()) {
      auto top = static_cast<IRTypeInstruction*>(insn);
      if (!ok_from_primary(top->get_type())) {
        return true;
      }
    } else if (insn->has_methods()) {
      auto mop = static_cast<IRMethodInstruction*>(insn);
      auto meth = mop->get_method();
      if (!ok_from_primary(meth->get_class())) {
        return true;
      }
      auto proto = meth->get_proto();
      if (!ok_from_primary(proto->get_rtype())) {
        return true;
      }
      auto args = proto->get_args();
      if (args == nullptr) continue;
      for (const auto& arg : args->get_type_list()) {
        if (!ok_from_primary(arg)) {
          return true;
        }
      }
    } else if (insn->has_fields()) {
      auto fop = static_cast<IRFieldInstruction*>(insn);
      auto field = fop->field();
      if (!ok_from_primary(field->get_class()) ||
          !ok_from_primary(field->get_type())) {
        return true;
      }
    }
  }
  return false;
}

/**
 * Change the visibility of members accessed in a callee as they are moved
 * to the caller context.
 * We make everything public but we could be more precise and only
 * relax visibility as needed.
 */
void MultiMethodInliner::change_visibility(DexMethod* callee) {
  TRACE(MMINL, 6, "checking visibility usage of members in %s\n",
      SHOW(callee));
  for (auto& mie : InstructionIterable(callee->get_code())) {
    auto insn = mie.insn;
    if (insn->has_fields()) {
      auto fop = static_cast<IRFieldInstruction*>(insn);
      auto field = fop->field();
      auto cls = type_class(field->get_class());
      if (cls != nullptr && !cls->is_external()) {
        set_public(cls);
      }
      field = resolve_field(field, is_sfield_op(insn->opcode())
          ? FieldSearch::Static : FieldSearch::Instance);
      if (field != nullptr && field->is_concrete()) {
        TRACE(MMINL, 6, "changing visibility of %s.%s %s\n",
            SHOW(field->get_class()), SHOW(field->get_name()),
            SHOW(field->get_type()));
        set_public(field);
        set_public(type_class(field->get_class()));
        // FIXME no point in rewriting opcodes in the callee
        fop->rewrite_field(field);
      }
      continue;
    }
    if (insn->has_methods()) {
      auto mop = static_cast<IRMethodInstruction*>(insn);
      auto method = mop->get_method();
      auto cls = type_class(method->get_class());
      if (cls != nullptr && !cls->is_external()) {
        set_public(cls);
      }
      method = resolver(method, opcode_to_search(insn));
      if (method != nullptr && method->is_concrete()) {
        TRACE(MMINL, 6, "changing visibility of %s.%s: %s\n",
            SHOW(method->get_class()), SHOW(method->get_name()),
            SHOW(method->get_proto()));
        set_public(method);
        set_public(type_class(method->get_class()));
        // FIXME no point in rewriting opcodes in the callee
        mop->rewrite_method(method);
      }
      continue;
    }
    if (insn->has_types()) {
      auto type = static_cast<IRTypeInstruction*>(insn)->get_type();
      auto cls = type_class(type);
      if (cls != nullptr && !cls->is_external()) {
        TRACE(MMINL, 6, "changing visibility of %s\n", SHOW(type));
        set_public(cls);
      }
      continue;
    }
  }

  std::vector<DexType*> types;
  callee->get_code()->gather_catch_types(types);
  for (auto type : types) {
    auto cls = type_class(type);
    if (cls != nullptr && !cls->is_external()) {
      TRACE(MMINL, 6, "changing visibility of %s\n", SHOW(type));
      set_public(cls);
    }
  }
}

namespace {

DexOpcode direct_to_static_op(DexOpcode op) {
  switch (op) {
    case OPCODE_INVOKE_DIRECT:
      return OPCODE_INVOKE_STATIC;
    case OPCODE_INVOKE_DIRECT_RANGE:
      return OPCODE_INVOKE_STATIC_RANGE;
    default:
      always_assert(false);
  }
}

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
  walk_opcodes(m_scope, [](DexMethod* meth) { return true; },
      [&](DexMethod* meth, IRInstruction* opcode) {
        auto op = opcode->opcode();
        if (op == OPCODE_INVOKE_DIRECT || op == OPCODE_INVOKE_DIRECT_RANGE) {
          auto mop = static_cast<IRMethodInstruction*>(opcode);
          if (m_make_static.count(mop->get_method())) {
            opcode->set_opcode(direct_to_static_op(op));
          }
        }
      });
}

void select_single_called(
    const Scope& scope,
    const std::unordered_set<DexMethod*>& methods,
    MethodRefCache& resolved_refs,
    std::unordered_set<DexMethod*>* inlinable) {
  std::unordered_map<DexMethod*, int> calls;
  for (const auto& method : methods) {
    calls[method] = 0;
  }
  // count call sites for each method
  walk_opcodes(scope, [](DexMethod* meth) { return true; },
      [&](DexMethod* meth, IRInstruction* insn) {
        if (is_invoke(insn->opcode())) {
          auto mop = static_cast<IRMethodInstruction*>(insn);
          auto callee = resolve_method(
              mop->get_method(), opcode_to_search(insn), resolved_refs);
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
    inlinable->insert(callee);
  }
}
