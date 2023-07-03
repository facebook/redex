/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "EnumUpcastAnalysis.h"

#include "EnumClinitAnalysis.h"
#include "Macros.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {

using namespace optimize_enums;
using namespace ir_analyzer;

bool need_analyze(const DexMethod* method,
                  const ConcurrentSet<DexType*>& candidate_enums,
                  const ConcurrentSet<DexType*>& rejected_enums) {
  const IRCode* code = method->get_code();
  if (!code) {
    return false;
  }
  std::vector<DexType*> types;
  method->gather_types(types);
  for (DexType* t : types) {
    if (type::is_array(t)) {
      t = type::get_array_element_type(t);
    }
    if (candidate_enums.count_unsafe(t) && !rejected_enums.count(t)) {
      return true;
    }
  }
  return false;
}

std::unordered_set<DexType*> discard_primitives(const EnumTypes& types) {
  std::unordered_set<DexType*> res;
  for (auto type : types.elements()) {
    if (!type::is_primitive(type)) {
      res.insert(type);
    }
  }
  return res;
}

/**
 * Inspect instructions to reject enum class that may be casted to another type.
 */
template <typename RejectFn>
class EnumUpcastDetector {
 public:
  EnumUpcastDetector(const DexMethod* method,
                     Config* config,
                     const RejectFn& reject_fn)
      : m_method(method),
        m_config(config),
        m_candidate_enums(&config->candidate_enums),
        m_reject_fn(reject_fn) {}

  void run(const EnumFixpointIterator& engine,
           const cfg::ControlFlowGraph& cfg) {
    for (const auto& block : cfg.blocks()) {
      EnumTypeEnvironment env = engine.get_entry_state_at(block);
      if (env.is_bottom()) {
        continue;
      }
      for (const auto& mie : InstructionIterable(block)) {
        engine.analyze_instruction(mie.insn, &env);
        process_instruction(mie.insn, &env);
      }
    }
  }

 private:
  /**
   * Process instructions when we reach the fixpoint.
   */
  void process_instruction(const IRInstruction* insn,
                           const EnumTypeEnvironment* env) {
    switch (insn->opcode()) {
    case OPCODE_CHECK_CAST: {
      auto type = insn->get_type();
      // Assume the local upcast is safe and we only care about upcasting when
      // the value is escaping.
      if (type != OBJECT_TYPE) {
        reject_if_inconsistent(insn, env->get(insn->src(0)), type,
                               UnsafeType::kUsageCastCheckCast);
      }
    } break;
    case OPCODE_CONST_CLASS:
      reject(insn, insn->get_type(), UnsafeType::kUsageUsedAsClassObject);
      break;
    case OPCODE_INSTANCE_OF:
      reject(insn, insn->get_type(), UnsafeType::kUsageUsedInInstanceOf);
      break;
    case OPCODE_INVOKE_INTERFACE:
    case OPCODE_INVOKE_SUPER:
      process_general_invocation(insn, env);
      break;
    case OPCODE_INVOKE_DIRECT:
      process_direct_invocation(insn, env);
      break;
    case OPCODE_INVOKE_STATIC:
      process_static_invocation(insn, env);
      break;
    case OPCODE_INVOKE_VIRTUAL:
      process_virtual_invocation(insn, env);
      break;
    case OPCODE_RETURN_OBJECT:
      process_return_object(insn, env);
      break;
    case OPCODE_APUT_OBJECT:
      process_aput_object(insn, env);
      break;
    case OPCODE_IPUT_OBJECT:
      process_isput_object(insn, env);
      reject(insn, insn->get_field()->get_class(),
             UnsafeType::kUsageIFieldSetOutsideInit);
      break;
    case OPCODE_IPUT:
    case OPCODE_IPUT_WIDE:
    case OPCODE_IPUT_BOOLEAN:
    case OPCODE_IPUT_BYTE:
    case OPCODE_IPUT_CHAR:
    case OPCODE_IPUT_SHORT:
      reject(insn, insn->get_field()->get_class(),
             UnsafeType::kUsageIFieldSetOutsideInit);
      break;
    case OPCODE_SPUT_OBJECT:
      process_isput_object(insn, env);
      break;
    default:
      break;
    }
  }

  /**
   * Process return-object instruction when we reach the fixpoint.
   */
  void process_return_object(const IRInstruction* insn,
                             const EnumTypeEnvironment* env) const {
    DexType* return_type = m_method->get_proto()->get_rtype();
    always_assert_log(env->get(insn->src(0)).is_value(),
                      "method %s\ninsn %s %s\n", SHOW(m_method), SHOW(insn),
                      SHOW(m_method->get_code()->cfg()));
    reject_if_inconsistent(insn, env->get(insn->src(0)), return_type,
                           UnsafeType::kUsageCastWhenReturn);
  }

  /**
   * Process iput-object and sput-object instructions when we reach the fix
   * point.
   */
  void process_isput_object(const IRInstruction* insn,
                            const EnumTypeEnvironment* env) const {
    auto arg_reg = insn->src(0);
    DexType* field_type = insn->get_field()->get_type();
    reject_if_inconsistent(insn, env->get(arg_reg), field_type,
                           UnsafeType::kUsageCastISPutObject);
  }

  /**
   * Process aput-object instruction when we reach the fixpoint.
   */
  void process_aput_object(const IRInstruction* insn,
                           const EnumTypeEnvironment* env) const {
    // It's possible that the array_types contains non-array types or is
    // array of primitives. Just ignore them.
    EnumTypes array_types = env->get(insn->src(1));
    EnumTypes elem_types = env->get(insn->src(0));
    std::unordered_set<DexType*> acceptable_elem_types;
    for (DexType* type : array_types.elements()) {
      DexType* elem = type::get_array_element_type(type);
      if (elem && !type::is_primitive(elem)) {
        acceptable_elem_types.insert(elem); // An array of one type of objects.
      }
    }
    if (acceptable_elem_types.size() > 1) {
      // If a register might be an array of multiple types, it's hard to do
      // further analysis so that we simply reject the types here.
      reject(insn, elem_types, UnsafeType::kUsageCastAputObject);
      reject(insn, acceptable_elem_types, UnsafeType::kUsageCastAputObject);
    } else if (acceptable_elem_types.size() == 1) {
      DexType* acceptable = *acceptable_elem_types.begin();
      reject_if_inconsistent(insn, elem_types, acceptable,
                             UnsafeType::kUsageCastAputObject);
    }
  }

  /**
   * No other direct invocation allowed on candidate enums except
   * candidate enum constructor invocations in the enum classes'
   * <clinit>.
   */
  void process_direct_invocation(const IRInstruction* insn,
                                 const EnumTypeEnvironment* env) const {
    always_assert(insn->opcode() == OPCODE_INVOKE_DIRECT);
    auto invoked = insn->get_method();
    auto container = invoked->get_class();
    if (m_candidate_enums->count_unsafe(container) &&
        method::is_init(invoked) && method::is_clinit(m_method)) {
      return;
    }
    process_general_invocation(insn, env);
  }

  /**
   * Analyze static method invocations if the invoked method is not
   * LString;.valueOf:(LObject;)LString;
   * LCandidateEnum;.valueOf:(String)LCandidateEnum; or
   * LCandidateEnum;.values:()[LCandidateEnum;
   *
   * Otherwise, figure out implicit parameter upcasting by adopting param
   * summary data.
   */
  void process_static_invocation(const IRInstruction* insn,
                                 const EnumTypeEnvironment* env) {
    always_assert(insn->opcode() == OPCODE_INVOKE_STATIC);
    auto method_ref = insn->get_method();
    if (method_ref == STRING_VALUEOF_METHOD) {
      EnumTypes types = env->get(insn->src(0));
      check_object_cast(types, insn);
      return;
    }
    auto container = method_ref->get_class();
    if (m_candidate_enums->count_unsafe(container)) {
      if (is_enum_values(method_ref) || is_enum_valueof(method_ref)) {
        return;
      }
    }
    auto method = resolve_method(method_ref, MethodSearch::Static);
    if (!method || !params_contain_object_type(method, OBJECT_TYPE)) {
      process_general_invocation(insn, env);
      return;
    }
    auto summary_it = m_config->param_summary_map.find(method);
    boost::optional<std::unordered_set<uint16_t>&> safe_params;
    if (summary_it != m_config->param_summary_map.end()) {
      auto& summary = summary_it->second;
      safe_params = summary.safe_params;
    }

    const auto args = method->get_proto()->get_args();
    auto it = args->begin();
    for (size_t arg_id = 0; arg_id < insn->srcs_size(); ++arg_id, ++it) {
      if (safe_params && safe_params->count(arg_id)) {
        continue;
      }
      reject_if_inconsistent(insn, env->get(insn->src(arg_id)), *it,
                             UnsafeType::kUsageCastParameter);
    }
  }

  /**
   * Process invoke-virtual instructions after we reach the fixpoint.
   *
   * But we can make assumptions for some methods although the invocations
   * seem to involve some cast operations.
   *
   *  # Enum.equals(Object) and Enum.compareTo(Enum) are final methods.
   *  INVOKE_VIRTUAL LCandidateEnum;.equals:(Ljava/lang/Object;)Z
   *  INVOKE_VIRTUAL LCandidateEnum;.compareTo:(Ljava/lang/Enum;)I
   *
   *  # We reject the candidate enum if it overrides `toString()` previously,
   *  # so the CandidateEnum.toString() is Enum.toString() and it behaves
   *  # the same as CandidateEnum.name().
   *  INVOKE_VIRTUAL LCandidateEnum;.toString:()String
   *  INVOKE_VIRTUAL LCandidateEnum;.name:()String
   *
   *  # When the Object param is a candidate enum object, the invocation can be
   *  * modeled.
   *  INVOKE_VIRTUAL StringBuilder.append:(Object)StringBuilder
   *
   *  # Other virtual invocations on candidate enum object that are considered
   *    safe.
   *  INVOKE_VIRTUAL ordinal:()I
   *  INVOKE_VIRTUAL hashCode:()I
   */
  void process_virtual_invocation(const IRInstruction* insn,
                                  const EnumTypeEnvironment* env) const {
    always_assert(insn->opcode() == OPCODE_INVOKE_VIRTUAL);
    const DexMethodRef* method = insn->get_method();
    DexType* container = method->get_class();

    // Class is Enum or a candidate enum class.
    if (container == ENUM_TYPE || m_candidate_enums->count_unsafe(container)) {
      EnumTypes a_types = env->get(insn->src(0));
      auto this_types = discard_primitives(a_types);
      // Method is equals or compareTo.
      if (method::signatures_match(method, ENUM_EQUALS_METHOD) ||
          method::signatures_match(method, ENUM_COMPARETO_METHOD)) {
        EnumTypes b_types = env->get(insn->src(1));
        auto that_types = discard_primitives(b_types);
        DexType* this_type = this_types.empty() ? nullptr : *this_types.begin();
        DexType* that_type = that_types.empty() ? nullptr : *that_types.begin();
        // Reject multiple types in the registers.
        if (this_types.size() > 1 || that_types.size() > 1 ||
            (this_type && that_type && this_type != that_type)) {
          reject(insn, this_types, UnsafeType::kUsageCastThisPointer);
          reject(insn, that_types, UnsafeType::kUsageCastParameter);
        }
        return;
      } else if (method::signatures_match(method, ENUM_TOSTRING_METHOD) ||
                 method::signatures_match(method, ENUM_HASHCODE_METHOD) ||
                 method::signatures_match(method, ENUM_NAME_METHOD) ||
                 method::signatures_match(method, ENUM_ORDINAL_METHOD)) {
        if (this_types.size() > 1) {
          reject(insn, this_types, UnsafeType::kUsageMultiEnumTypes);
        }
        return;
      }
    } else if (method == STRINGBUILDER_APPEND_METHOD) {
      EnumTypes b_types = env->get(insn->src(1));
      check_object_cast(b_types, insn);
      return;
    }
    // If not special cases, do the general processing.
    process_general_invocation(insn, env);
  }

  /**
   * Analyze invoke instruction's arguments, if the type of arguments are not
   * consistent with the method signature, reject these types.
   */
  void process_general_invocation(const IRInstruction* insn,
                                  const EnumTypeEnvironment* env) const {
    always_assert(insn->has_method());
    auto method = insn->get_method();
    auto proto = method->get_proto();
    auto container = method->get_class();
    // Check the type of arguments.
    auto* args = proto->get_args();
    always_assert(args->size() == insn->srcs_size() ||
                  args->size() == insn->srcs_size() - 1);
    size_t arg_id = 0;
    if (insn->srcs_size() == args->size() + 1) {
      // this pointer
      reject_if_inconsistent(insn, env->get(insn->src(arg_id)), container,
                             UnsafeType::kUsageCastThisPointer);
      arg_id++;
    }
    // Arguments
    auto it = args->begin();
    for (; arg_id < insn->srcs_size(); ++arg_id, ++it) {
      reject_if_inconsistent(insn, env->get(insn->src(arg_id)), *it,
                             UnsafeType::kUsageCastParameter);
    }
  }

  bool is_candidate(DexType* type) const {
    if (type == nullptr) {
      return false;
    }
    type = const_cast<DexType*>(type::get_element_type_if_array(type));
    return m_candidate_enums->count_unsafe(type);
  }

  void check_object_cast(const EnumTypes& types,
                         const IRInstruction* insn) const {
    auto that_types = discard_primitives(types);
    if (that_types.size() > 1) {
      reject(insn, that_types, UnsafeType::kUsageMultiEnumTypes);
    } else if (that_types.size() == 1 && type::is_array(*that_types.begin())) {
      reject(insn, that_types, UnsafeType::kUsageCastEnumArrayToObject);
    }
  }

  /**
   * If types of register is not consistent with required_type, remove these
   * types from candidate enum set.
   */
  void reject_if_inconsistent(const IRInstruction* insn,
                              const EnumTypes& types,
                              DexType* required_type,
                              UnsafeType reason = UnsafeType::kUsage) const {
    if (is_candidate(required_type)) {
      bool need_delete = false;
      for (auto possible_type : types.elements()) {
        if (!type::is_primitive(possible_type) &&
            possible_type != required_type) {
          need_delete = true;
          reject(insn, possible_type, reason);
        }
      }
      if (need_delete) {
        reject(insn, required_type, reason);
      }
    } else {
      for (auto possible_type : types.elements()) {
        reject(insn, possible_type, reason);
      }
    }
  }

  void reject(const IRInstruction* insn,
              const std::unordered_set<DexType*>& types,
              UnsafeType reason = UnsafeType::kUsage) const {
    for (DexType* type : types) {
      reject(insn, type, reason);
    }
  }

  void reject(const IRInstruction* insn,
              const EnumTypes& types,
              UnsafeType reason = UnsafeType::kUsage) const {
    for (DexType* type : types.elements()) {
      reject(insn, type, reason);
    }
  }

  void reject(const IRInstruction* insn,
              DexType* type,
              UnsafeType reason = UnsafeType::kUsage) const {
    type = const_cast<DexType*>(type::get_element_type_if_array(type));
    if (m_candidate_enums->count_unsafe(type)) {
      m_reject_fn(type, reason);
      TRACE(ENUM, 9, "reject %s %d %s %s", SHOW(type), (int)reason,
            SHOW(m_method), SHOW(insn));
    }
  }

  const DexMethodRef* ENUM_EQUALS_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.equals:(Ljava/lang/Object;)Z");
  const DexMethodRef* ENUM_COMPARETO_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.compareTo:(Ljava/lang/Enum;)I");
  const DexMethodRef* ENUM_TOSTRING_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.toString:()Ljava/lang/String;");
  const DexMethodRef* ENUM_HASHCODE_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.hashCode:()I");
  const DexMethodRef* ENUM_NAME_METHOD = method::java_lang_Enum_name();
  const DexMethodRef* ENUM_ORDINAL_METHOD = method::java_lang_Enum_ordinal();
  const DexMethodRef* STRING_VALUEOF_METHOD = DexMethod::make_method(
      "Ljava/lang/String;.valueOf:(Ljava/lang/Object;)Ljava/lang/String;");
  const DexMethodRef* STRINGBUILDER_APPEND_METHOD = DexMethod::make_method(
      "Ljava/lang/StringBuilder;.append:(Ljava/lang/Object;)Ljava/lang/"
      "StringBuilder;");
  const DexType* ENUM_TYPE = type::java_lang_Enum();
  const DexType* OBJECT_TYPE = type::java_lang_Object();
  const DexType* STRING_TYPE = type::java_lang_String();

  const DexMethod* m_method;
  Config* m_config;
  const ConcurrentSet<DexType*>* m_candidate_enums;
  const RejectFn& m_reject_fn;
};

bool is_static_method_on_enum_class(const DexMethodRef* ref) {
  auto method = static_cast<const DexMethod*>(ref);
  if (!method || !method->is_def() || !is_static(method)) {
    return false;
  }
  auto container = type_class(method->get_class());
  return container && is_enum(container);
}
} // namespace

namespace optimize_enums {

/**
 * Analyze all the instructions that may involve object or type and handle
 * possible candidate enums specificly.
 */
void EnumFixpointIterator::analyze_instruction(const IRInstruction* insn,
                                               EnumTypeEnvironment* env) const {
  const bool use_result = insn->has_move_result_any();
  if (use_result || insn->has_dest()) {
    reg_t dest = use_result ? RESULT_REGISTER : insn->dest();

    switch (insn->opcode()) {
    case IOPCODE_LOAD_PARAM:
    case IOPCODE_LOAD_PARAM_OBJECT:
    case IOPCODE_LOAD_PARAM_WIDE:
      // Parameters are processed before we run FixpointIterator
      break;
    case OPCODE_MOVE_OBJECT:
      env->set(dest, env->get(insn->src(0)));
      break;
    case OPCODE_INVOKE_STATIC: {
      auto method = resolve_method(insn->get_method(), MethodSearch::Static);
      if (method) {
        auto it = m_config.param_summary_map.find(method);
        if (it != m_config.param_summary_map.end()) {
          const auto& summary = it->second;
          if (summary.returned_param) {
            auto returned = summary.returned_param.get();
            if (summary.safe_params.count(returned)) {
              env->set(dest, env->get(insn->src(returned)));
              return;
            }
          }
        }
      }
      env->set(dest, EnumTypes(insn->get_method()->get_proto()->get_rtype()));
    } break;
    case OPCODE_INVOKE_SUPER:
    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_INTERFACE:
    case OPCODE_INVOKE_VIRTUAL:
      env->set(dest, EnumTypes(insn->get_method()->get_proto()->get_rtype()));
      break;
    case OPCODE_CONST_CLASS:
      env->set(dest, EnumTypes(type::java_lang_Class()));
      break;
    case OPCODE_CHECK_CAST: {
      auto type = insn->get_type();
      if (type == OBJECT_TYPE) {
        env->set(dest, env->get(insn->src(0)));
      } else {
        env->set(dest, EnumTypes(type));
      }
    } break;
    case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
    case OPCODE_MOVE_RESULT_OBJECT:
      env->set(dest, env->get(RESULT_REGISTER));
      break;
    case OPCODE_SGET_OBJECT:
    case OPCODE_IGET_OBJECT: {
      DexType* type = insn->get_field()->get_type();
      if (!type::is_primitive(type)) {
        env->set(dest, EnumTypes(type));
      }
    } break;
    case OPCODE_AGET_OBJECT: {
      EnumTypes types;
      EnumTypes array_types = env->get(insn->src(0));
      for (auto const array_type : array_types.elements()) {
        const auto type = type::get_array_element_type(array_type);
        if (type && !type::is_primitive(type)) {
          types.add(type);
        }
      }
      env->set(dest, types);
    } break;
    case OPCODE_NEW_ARRAY:
    case OPCODE_NEW_INSTANCE:
    case OPCODE_FILLED_NEW_ARRAY:
    case OPCODE_CONST_STRING: // We don't care about string object
    default:
      if (insn->has_type()) {
        env->set(dest, EnumTypes(insn->get_type()));
      } else {
        env->set(dest, EnumTypes());
      }
      // When we write a wide register v, the v+1 register is overrode.
      if (insn->has_dest() && insn->dest_is_wide()) {
        env->set(dest + 1, EnumTypes());
      }
      break;
    }
  }
}

/**
 * Generate environment with method parameter registers.
 */
EnumTypeEnvironment EnumFixpointIterator::gen_env(const DexMethod* method) {
  EnumTypeEnvironment env;
  const auto params = method->get_code()->cfg().get_param_instructions();
  const DexTypeList* args = method->get_proto()->get_args();
  const bool has_this_pointer = !is_static(method);
  size_t load_param_inst_size = 0;
  for (const auto& mie ATTRIBUTE_UNUSED : InstructionIterable(params)) {
    ++load_param_inst_size;
  }
  always_assert(load_param_inst_size == args->size() + has_this_pointer);

  auto iterable = InstructionIterable(params);
  auto it = iterable.begin();
  if (has_this_pointer) { // Has this pointer
    env.set(it->insn->dest(), EnumTypes(method->get_class()));
    ++it;
  }
  for (DexType* type : *args) {
    env.set(it->insn->dest(), EnumTypes(type));
    ++it;
  }
  return env;
}

/**
 * Reject enums that would cause constructors to have prototypes that would
 * become identical.
 */
void reject_enums_for_colliding_constructors(
    const std::vector<DexClass*>& classes,
    ConcurrentSet<DexType*>* candidate_enums) {
  ConcurrentSet<DexType*> rejected_enums;

  walk::parallel::classes(classes, [&](DexClass* cls) {
    const auto& ctors = cls->get_ctors();
    if (ctors.size() <= 1) {
      return;
    }
    std::unordered_set<DexTypeList*> modified_params_lists;
    for (auto ctor : ctors) {
      std::unordered_set<DexType*> transforming_enums;
      DexTypeList::ContainerType param_types{
          ctor->get_proto()->get_args()->begin(),
          ctor->get_proto()->get_args()->end()};
      for (size_t i = 0; i < param_types.size(); i++) {
        auto base_type = const_cast<DexType*>(
            type::get_element_type_if_array(param_types[i]));
        if (candidate_enums->count(base_type)) {
          transforming_enums.insert(base_type);
          param_types[i] = type::make_array_type(
              type::java_lang_Integer(), type::get_array_level(param_types[i]));
        }
      }
      auto new_params = DexTypeList::make_type_list(std::move(param_types));
      if (modified_params_lists.count(new_params)) {
        for (auto enum_type : transforming_enums) {
          TRACE(ENUM, 4,
                "Reject %s because it would create a method prototype "
                "collision for %s",
                SHOW(enum_type), SHOW(ctor));
          rejected_enums.insert(enum_type);
        }
      } else {
        auto new_proto = DexProto::make_proto(type::_void(), new_params);
        if (DexMethod::get_method(ctor->get_class(), ctor->get_name(),
                                  new_proto) != nullptr) {
          for (auto enum_type : transforming_enums) {
            TRACE(ENUM, 4,
                  "Reject %s because it would create a method prototype "
                  "collision for %s",
                  SHOW(enum_type), SHOW(ctor));
            rejected_enums.insert(enum_type);
          }
        } else {
          modified_params_lists.insert(new_params);
        }
      }
    }
  });

  for (DexType* type : rejected_enums) {
    candidate_enums->erase(type);
  }
}

void reject_unsafe_enums(
    const std::vector<DexClass*>& classes,
    Config* config,
    const std::function<void(const DexType*, UnsafeType u)>& reject_fn) {
  auto candidate_enums = &config->candidate_enums;
  ConcurrentSet<DexType*> rejected_enums;

  walk::parallel::fields(
      classes, [candidate_enums, &rejected_enums, &reject_fn](DexField* field) {
        if (can_rename(field)) {
          return;
        }
        if (candidate_enums->count_unsafe(field->get_class())) {
          auto access = field->get_access();
          if (check_required_access_flags(enum_field_access(), access) ||
              check_required_access_flags(synth_access(), access)) {
            return;
          }
        }
        auto type = const_cast<DexType*>(
            type::get_element_type_if_array(field->get_type()));
        if (candidate_enums->count_unsafe(type)) {
          rejected_enums.insert(type);
          reject_fn(type, UnsafeType::kUsageUnrenamableFieldType);
        }
      });

  walk::parallel::methods(classes, [&](DexMethod* method) {
    // When doing static analysis, simply skip some javac-generated enum
    // methods <init>, values(), and valueOf(String).
    if (candidate_enums->count_unsafe(method->get_class()) &&
        !rejected_enums.count(method->get_class()) &&
        (method::is_init(method) || is_enum_values(method) ||
         is_enum_valueof(method))) {
      return;
    }

    auto reject_proto_types = [&](DexMethod* method, UnsafeType u) {
      std::vector<DexType*> types;
      method->get_proto()->gather_types(types);
      for (auto type : types) {
        auto elem_type =
            const_cast<DexType*>(type::get_element_type_if_array(type));
        if (candidate_enums->count_unsafe(elem_type)) {
          TRACE(ENUM, 5,
                "Rejecting %s due to !can_rename or usage from annotation",
                SHOW(elem_type));
          reject_fn(elem_type, u);
          rejected_enums.insert(elem_type);
        }
      }
    };

    if (!can_rename(method)) {
      reject_proto_types(method, UnsafeType::kUsageUnrenamableMethodRef);
      if (!is_static(method) &&
          candidate_enums->count_unsafe(method->get_class())) {
        rejected_enums.insert(method->get_class());
      }
    }

    auto method_cls = type_class(method->get_class());
    if (method_cls != nullptr && is_annotation(method_cls)) {
      reject_proto_types(method, UnsafeType::kUsageAnnotationMethodRef);
    }

    if (!need_analyze(method, *candidate_enums, rejected_enums)) {
      return;
    }

    auto& cfg = method->get_code()->cfg();
    EnumTypeEnvironment env = EnumFixpointIterator::gen_env(method);
    EnumFixpointIterator engine(cfg, *config);
    engine.run(env);

    auto local_reject_fn = [&](DexType* type, UnsafeType u) {
      rejected_enums.insert(type);
      reject_fn(type, u);
    };

    EnumUpcastDetector detector(method, config, local_reject_fn);
    detector.run(engine, cfg);
  });

  for (DexType* type : rejected_enums) {
    candidate_enums->erase(type);
  }

  reject_enums_for_colliding_constructors(classes, candidate_enums);
}

bool is_enum_valueof(const DexMethodRef* method) {
  if (!is_static_method_on_enum_class(method) || method->str() != "valueOf") {
    return false;
  }
  auto proto = method->get_proto();
  if (method->get_class() != proto->get_rtype()) {
    return false;
  }
  auto* args = proto->get_args();
  return args->size() == 1 && args->at(0) == type::java_lang_String();
}

bool is_enum_values(const DexMethodRef* method) {
  if (!is_static_method_on_enum_class(method) || method->str() != "values") {
    return false;
  }
  auto proto = method->get_proto();
  if (!proto->get_args()->empty()) {
    return false;
  }
  return type::get_array_component_type(proto->get_rtype()) ==
         method->get_class();
}

} // namespace optimize_enums
