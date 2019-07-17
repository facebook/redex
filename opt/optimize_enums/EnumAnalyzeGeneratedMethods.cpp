/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "EnumAnalyzeGeneratedMethods.h"
#include "DexAsm.h" /* rewrite_method_to_throw_error */

using namespace optimize_enums;

/**
 * This is used just to make sure that the generated methods can be removed.
 *
 * foo(...) {
 *   throw RuntimeException("...");
 * }
 */
void rewrite_method_to_throw_error(DexMethod* method,
                                   const std::string& enum_name) {
  using namespace dex_asm;
  auto code = method->get_code();
  code->build_cfg();
  auto& cfg = code->cfg();
  auto entry = cfg.entry_block();
  auto first_non_param_insn = entry->to_cfg_instruction_iterator(
      entry->get_first_non_param_loading_insn());
  static DexType* error_type =
      DexType::get_type("Ljava/lang/RuntimeException;");
  static DexMethodRef* error_constructor_method = DexMethod::get_method(
      "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V");
  always_assert(error_type && error_constructor_method);
  entry->insert_before(
      first_non_param_insn,
      {dasm(OPCODE_NEW_INSTANCE, error_type, {}),
       dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {2_v}),
       dasm(OPCODE_CONST_STRING,
            DexString::make_string(method->get_simple_deobfuscated_name() +
                                   " removed from " + enum_name)),
       dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {3_v}),
       dasm(OPCODE_INVOKE_DIRECT, error_constructor_method, {2_v, 3_v}),
       dasm(OPCODE_THROW, {2_v})});
  cfg.recompute_registers_size();
  // Unreached blocks will be deleted.
  code->clear_cfg();
}

size_t EnumAnalyzeGeneratedMethods::transform_code(const Scope& scope) {

  walk::parallel::code(scope, [this](DexMethod* method, IRCode& code) {
    // Do not analyze the generated methods we are trying to remove.
    if (is_enum_valueof(method) || is_enum_values(method)) {
      return;
    }

    auto env = EnumFixpointIterator::gen_env(method);
    code.build_cfg(/*editable*/ false);
    EnumFixpointIterator engine(code.cfg(), m_config);
    engine.run(env);

    process_method(engine, code.cfg(), method);
  });

  size_t num_removed_methods = 0;
  for (auto candidate_method : m_candidate_methods) {
    const DexType* candidate_type = candidate_method->get_class();
    if (m_candidate_types.count_unsafe(candidate_type)) {
      DexClass* candidate_class = type_class(candidate_type);
      always_assert(candidate_class);
      TRACE(ENUM, 4, "safe to remove method %s from %s", SHOW(candidate_method),
            SHOW(candidate_class));
      // candidate_class->remove_method(candidate_method);
      rewrite_method_to_throw_error(candidate_method, candidate_type->str());
      num_removed_methods++;
    }
  }

  return num_removed_methods;
}

void EnumAnalyzeGeneratedMethods::process_method(
    const EnumFixpointIterator& engine,
    const cfg::ControlFlowGraph& cfg,
    const DexMethod* method) {
  for (const auto& block : cfg.blocks()) {
    EnumTypeEnvironment env = engine.get_entry_state_at(block);
    if (env.is_bottom()) {
      continue;
    }
    for (const auto& mie : InstructionIterable(block)) {
      engine.analyze_instruction(mie.insn, &env);
      process_instruction(mie.insn, &env, method);
    }
  }
}

/**
 * Rejects all enums that try to use its class type and all enums that are
 * upcasted and escape a method. This can happen by returning an upcasted enum
 * or by assigning it to some field or array.
 */
void EnumAnalyzeGeneratedMethods::process_instruction(
    const IRInstruction* insn,
    const EnumTypeEnvironment* env,
    const DexMethod* method) {
  switch (insn->opcode()) {
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE:
    process_invocation(insn, env);
    break;
  case OPCODE_CHECK_CAST:
  case OPCODE_CONST_CLASS: {
    auto type = get_array_type_or_self(insn->get_type());
    if (m_candidate_types.count(type)) {
      TRACE(ENUM, 4, "reject enum %s for using class type", SHOW(type));
      m_candidate_types.erase(type);
    }
  } break;
  case OPCODE_FILLED_NEW_ARRAY: {
    auto base_type = get_array_type(insn->get_type());
    always_assert(base_type);
    for (size_t src_id = 1; src_id < insn->srcs_size(); src_id++) {
      const EnumTypes elem_types = env->get(insn->src(src_id));
      reject_if_unsafe(base_type, elem_types, insn);
    }
  } break;
  case OPCODE_APUT_OBJECT: {
    const EnumTypes elem_types = env->get(insn->src(0));
    const EnumTypes array_types = env->get(insn->src(1));
    for (const DexType* escaping_type : array_types.elements()) {
      auto base_escaping_type = get_array_type_or_self(escaping_type);
      reject_if_unsafe(base_escaping_type, elem_types, insn);
    }
  } break;
  case OPCODE_IPUT_OBJECT:
  case OPCODE_SPUT_OBJECT: {
    auto type = get_array_type_or_self(insn->get_field()->get_type());
    reject_if_unsafe(type, env->get(insn->src(0)), insn);
  } break;
  case OPCODE_RETURN_OBJECT: {
    auto return_type = get_array_type_or_self(method->get_proto()->get_rtype());
    reject_if_unsafe(return_type, env->get(insn->src(0)), insn);
  } break;
  default:
    break;
  }
}

/**
 * We reject an enum method if it is invoked. We also reject enums if invokes
 * the method `Enum.getDeclaringClass()` or a method whose arguments would
 * upcast that enum.
 */
void EnumAnalyzeGeneratedMethods::process_invocation(
    const IRInstruction* insn, const EnumTypeEnvironment* env) {
  auto callee_ref = insn->get_method();
  auto callee_class = callee_ref->get_class();
  auto proto = callee_ref->get_proto();

  /**
   * A list of common enum methods that may upcast arguments, but cannot lead
   * to a call to `Enum.valueOf()` or `Enum.values()` directly or reflectively
   * because they are safe and final.
   */
  static const DexMethodRef* whitelisted_methods[] = {
      DexMethod::get_method("Ljava/lang/Enum;.<init>:(Ljava/lang/String;I)V"),
      DexMethod::get_method("Ljava/lang/Enum;.compareTo:(Ljava/lang/Enum;)I"),
      DexMethod::get_method("Ljava/lang/Enum;.equals:(Ljava/lang/Object;)Z"),
      DexMethod::get_method("Ljava/lang/Enum;.hashCode:()I"),
      DexMethod::get_method("Ljava/lang/Enum;.name:()Ljava/lang/String;"),
      DexMethod::get_method("Ljava/lang/Enum;.ordinal:()I"),
      DexMethod::get_method("Ljava/lang/Enum;.toString:()Ljava/lang/String;"),
  };

  if (m_candidate_types.count(callee_class) ||
      get_enum_type() == callee_class || get_object_type() == callee_class) {
    for (auto whitelisted_method : whitelisted_methods) {
      if (signatures_match(callee_ref, whitelisted_method)) {
        TRACE(ENUM, 9, "Skipping whitelisted invocation %s", SHOW(insn));
        return;
      }
    }
  }

  // `param_id` is the id of the parameter in the callee prototype
  size_t param_id = 0;
  // `arg_id` is the id of the argument in the invocation
  size_t arg_id = 0;
  if (insn->opcode() != OPCODE_INVOKE_STATIC) {
    // The first argument is the `this` pointer.
    reject_if_unsafe(callee_ref->get_class(), env->get(insn->src(arg_id)),
                     insn);
    arg_id++;
  }
  const DexTypeList* parameters = proto->get_args();
  while (param_id < parameters->size()) {
    const EnumTypes possible_types = env->get(insn->src(arg_id));
    reject_if_unsafe(get_array_type_or_self(parameters->at(param_id)),
                     possible_types, insn);
    param_id++;
    arg_id++;
  }

  switch (insn->opcode()) {
  case OPCODE_INVOKE_VIRTUAL: {
    const DexMethodRef* get_declaring_class_method = DexMethod::get_method(
        "Ljava/lang/Enum;.getDeclaringClass:()Ljava/lang/Class;");
    if (signatures_match(callee_ref, get_declaring_class_method)) {
      const EnumTypes possible_types = env->get(insn->src(0));
      for (const DexType* type : possible_types.elements()) {
        if (m_candidate_types.count(type)) {
          TRACE(ENUM, 4, "reject enum %s for using class type", SHOW(type));
          m_candidate_types.erase(type);
        }
      }
    }
  } break;
  case OPCODE_INVOKE_STATIC: {
    auto callee = resolve_method(callee_ref, MethodSearch::Static);
    if (m_candidate_methods.count(callee)) {
      if (is_enum_valueof(callee)) {
        // Enum.valueOf() calls Enum.values() and so we reject the whole type
        DexType* callee_type = callee->get_class();
        TRACE(ENUM, 4, "reject enum %s for calling valueOf", SHOW(callee_type));
        m_candidate_types.erase(callee_type);
      } else { // `SubEnum.values()`
        TRACE(ENUM, 4, "reject generated enum method %s", SHOW(callee));
        m_candidate_methods.erase(callee);
      }
    }
  } break;
  default:
    break;
  }
}

void EnumAnalyzeGeneratedMethods::reject_if_unsafe(
    const DexType* expected_type,
    const EnumTypes& possible_types,
    const IRInstruction* insn) {
  for (const DexType* possible_type : possible_types.elements()) {
    auto type = get_array_type_or_self(possible_type);
    if (expected_type != type && m_candidate_types.count(type)) {
      TRACE(ENUM, 4, "reject enum %s for upcasting to %s in %s", SHOW(type),
            SHOW(expected_type), SHOW(insn));
      m_candidate_types.erase(type);
    }
  }
}
