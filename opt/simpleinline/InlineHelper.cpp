/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "InlineHelper.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "Mutators.h"
#include "Resolver.h"
#include "walkers.h"

namespace {

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
    DexClasses& primary_dex,
    std::unordered_set<DexMethod*>& candidates,
    std::function<DexMethod*(DexMethod*, MethodSearch)> resolver,
    const Config& config)
    : resolver(resolver), m_scope(scope), m_config(config) {
  for (const auto& cls : primary_dex) {
    primary.insert(cls->get_type());
  }
  // walk every opcode in scope looking for calls to inlinable candidates
  // and build a map of callers to callees and the reverse callees to callers
  walk_opcodes(scope, [](DexMethod* meth) { return true; },
      [&](DexMethod* meth, DexInstruction* opcode) {
        if (is_invoke(opcode->opcode())) {
          auto mop = static_cast<DexOpcodeMethod*>(opcode);
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
  // save all changes made
  MethodTransform::sync_all();

  invoke_direct_to_static();
}

void MultiMethodInliner::caller_inline(
    DexMethod* caller,
    std::vector<DexMethod*>& callees,
    std::unordered_set<DexMethod*>& visited) {
  // recurse into the callees in case they have something to inline on
  // their own. We want to inline bottom up so that a callee is
  // completely resolved by the time it is inlined.
  for (auto callee : callees) {
    // if the call chain hits a call loop, ignore and keep going
    if (visited.count(callee) > 0) {
      info.recursive++;
      continue;
    }
    auto maybe_caller = caller_callee.find(callee);
    if (maybe_caller != caller_callee.end()) {
      visited.insert(callee);
      caller_inline(callee, maybe_caller->second, visited);
    }
  }
  if (!m_config.try_catch_inline && caller->get_code()->get_tries().size() > 0) {
    info.caller_tries++;
    return;
  }
  InlineContext inline_context(caller);
  inline_callees(inline_context, callees);
}

void MultiMethodInliner::inline_callees(
    InlineContext& inline_context, std::vector<DexMethod*>& callees) {
  size_t found = 0;
  auto caller = inline_context.caller;
  auto insns = caller->get_code()->get_instructions();

  // walk the caller opcodes collecting all candidates to inline
  // Build a callee to opcode map
  std::vector<std::pair<DexMethod*, DexOpcodeMethod*>> inlinables;
  for (auto insn = insns.begin(); insn != insns.end(); ++insn) {
    if (!is_invoke((*insn)->opcode())) continue;
    auto mop = static_cast<DexOpcodeMethod*>(*insn);
    auto callee = resolver(mop->get_method(), opcode_to_search(*insn));
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
  for (auto inlinable : inlinables) {
    auto callee = inlinable.first;
    auto mop = inlinable.second;

    if (!is_inlinable(callee, caller)) continue;

    TRACE(MMINL, 4, "inline %s (%d) in %s (%d)\n",
        SHOW(callee), caller->get_code()->get_registers_size(),
        SHOW(caller),
        callee->get_code()->get_registers_size() -
        callee->get_code()->get_ins_size());
    change_visibility(callee);
    MethodTransform::inline_16regs(inline_context, callee, mop);
    info.calls_inlined++;
    inlined.insert(callee);
  }
}

/**
 * Defines the set of rules that determine whether a function is inlinable.
 */
bool MultiMethodInliner::is_inlinable(DexMethod* callee, DexMethod* caller) {
  // don't bring anything into primary that is not in primary
  if (primary.count(caller->get_class()) != 0 && refs_not_in_primary(callee)) {
    return false;
  }
  if (is_enum_method(callee)) return false;
  if (over_16regs(caller, callee)) return false;
  if (!m_config.try_catch_inline && has_try_catch(callee)) return false;

  if (cannot_inline_opcodes(callee, caller)) return false;

  return true;
}

/**
 * Return whether the method is in an Enum class.
 * Enum methods are invoked in different magic ways and should never
 * be removed.
 */
bool MultiMethodInliner::is_enum_method(DexMethod* callee) {
  if (type_class(callee->get_class())->get_super_class() == get_enum_type()) {
    info.enum_callee++;
    return true;
  }
  return false;
}

/**
 * Return whether the number of registers to add to the caller, in order to
 * accommodate the callee, would spill over 16 registers.
 * More than 16 registers require special bytecodes for some operations and we
 * do not manage it now.
 */
bool MultiMethodInliner::over_16regs(DexMethod* caller, DexMethod* callee) {
  auto regs = caller->get_code()->get_registers_size();
  regs += (callee->get_code()->get_registers_size() -
      callee->get_code()->get_ins_size());
  if (regs > 16) {
    info.more_than_16regs++;
    return true;
  }
  return false;
}

/**
 * Return whether the callee has any try/catch.
 * We don't inline try/catch for now but it should be relatively trivial
 * to do so.
 */
bool MultiMethodInliner::has_try_catch(DexMethod* callee) {
  if (callee->get_code()->get_tries().size() > 0) {
    info.try_catch_block++;
    return true;
  }
  return false;
}

/**
 * Analyze opcodes in the callee to see if they are problematic for inlining.
 */
bool MultiMethodInliner::cannot_inline_opcodes(DexMethod* callee,
                                               DexMethod* caller) {
  int ret_count = 0;
  auto code = callee->get_code();
  uint16_t temp_regs =
      static_cast<uint16_t>(code->get_registers_size() - code->get_ins_size());
  for (auto insn : callee->get_code()->get_instructions()) {
    if (create_vmethod(insn)) return true;
    if (is_invoke_super(insn)) return true;
    if (writes_ins_reg(insn, temp_regs)) return true;
    if (unknown_virtual(insn, callee, caller)) return true;
    if (unknown_field(insn, callee)) return true;
    if (insn->opcode() == OPCODE_THROW) {
      info.throws++;
      return true;
    }
    if (insn->opcode() == FOPCODE_FILLED_ARRAY) {
      info.array_data++;
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
bool MultiMethodInliner::create_vmethod(DexInstruction* insn) {
  auto opcode = insn->opcode();
  if (opcode == OPCODE_INVOKE_DIRECT || opcode == OPCODE_INVOKE_DIRECT_RANGE) {
    auto method = static_cast<DexOpcodeMethod*>(insn)->get_method();
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
        !(method->get_access() & ACC_NATIVE)) {
      m_make_static.insert(method);
    } else {
      info.need_vmethod++;
      return true;
    }
  }
  return false;
}

/**
 * Return whether the callee contains an invoke-super.
 * Inlining an invoke_super off its class hierarchy would break the verifier.
 */
bool MultiMethodInliner::is_invoke_super(DexInstruction* insn) {
  if (insn->opcode() == OPCODE_INVOKE_SUPER ||
      insn->opcode() == OPCODE_INVOKE_SUPER_RANGE) {
    info.invoke_super++;
    return true;
  }
  return false;
}

/**
 * Return whether the callee contains a check-cast to or writes one of the ins.
 * When inlining writing over one of the ins may change the type of the
 * register to a type that breaks the invariants in the caller.
 */
bool MultiMethodInliner::writes_ins_reg(DexInstruction* insn, uint16_t temp_regs) {
  int reg = -1;
  if (insn->opcode() == OPCODE_CHECK_CAST) {
    reg = insn->src(0);
  } else if (insn->dests_size() > 0) {
    reg = insn->dest();
  }
  // temp_regs are the first n registers in the method that are not ins.
  // Dx methods use the last k registers for the arguments (where k is the size
  // of the args).
  // So an instruction writes an ins if it has a destination and the
  // destination is bigger or equal than temp_regs (0 is a reg).
  if (reg >= 0 && static_cast<uint16_t>(reg) >= temp_regs) {
    info.write_over_ins++;
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

bool MultiMethodInliner::unknown_virtual(DexInstruction* insn,
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
    auto method = static_cast<DexOpcodeMethod*>(insn)->get_method();
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
bool MultiMethodInliner::unknown_field(DexInstruction* insn, DexMethod* context) {
  if (is_ifield_op(insn->opcode()) || is_sfield_op(insn->opcode())) {
    auto fop = static_cast<DexOpcodeField*>(insn);
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

  for (auto insn : callee->get_code()->get_instructions()) {
    if (insn->has_types()) {
      auto top = static_cast<DexOpcodeType*>(insn);
      if (!ok_from_primary(top->get_type())) {
        return true;
      }
    } else if (insn->has_methods()) {
      auto mop = static_cast<DexOpcodeMethod*>(insn);
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
      auto fop = static_cast<DexOpcodeField*>(insn);
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
  for (auto insn : callee->get_code()->get_instructions()) {
    if (insn->has_fields()) {
      auto fop = static_cast<DexOpcodeField*>(insn);
      auto field = fop->field();
      field = resolve_field(field, is_sfield_op(insn->opcode())
          ? FieldSearch::Static : FieldSearch::Instance);
      if (field != nullptr && field->is_concrete()) {
        TRACE(MMINL, 6, "changing visibility of %s.%s %s\n",
            SHOW(field->get_class()), SHOW(field->get_name()),
            SHOW(field->get_type()));
        set_public(field);
        set_public(type_class(field->get_class()));
        fop->rewrite_field(field);
      }
      continue;
    }
    if (insn->has_methods()) {
      auto mop = static_cast<DexOpcodeMethod*>(insn);
      auto method = mop->get_method();
      method = resolver(method, opcode_to_search(insn));
      if (method != nullptr && method->is_concrete()) {
        TRACE(MMINL, 6, "changing visibility of %s.%s: %s\n",
            SHOW(method->get_class()), SHOW(method->get_name()),
            SHOW(method->get_proto()));
        set_public(method);
        set_public(type_class(method->get_class()));
        mop->rewrite_method(method);
      }
      continue;
    }
    if (insn->has_types()) {
      auto type = static_cast<DexOpcodeType*>(insn)->get_type();
      auto cls = type_class(type);
      if (cls != nullptr && !cls->is_external()) {
        TRACE(MMINL, 6, "changing visibility of %s\n", SHOW(type));
        set_public(cls);
      }
      continue;
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
  for (auto method : m_make_static) {
    TRACE(MMINL, 6, "making %s static\n", method->get_name()->c_str());
    mutators::make_static(method);
  }
  walk_opcodes(m_scope, [](DexMethod* meth) { return true; },
      [&](DexMethod* meth, DexInstruction* opcode) {
        auto op = opcode->opcode();
        if (op == OPCODE_INVOKE_DIRECT || op == OPCODE_INVOKE_DIRECT_RANGE) {
          auto mop = static_cast<DexOpcodeMethod*>(opcode);
          if (m_make_static.count(mop->get_method())) {
            opcode->set_opcode(direct_to_static_op(op));
          }
        }
      });
}
