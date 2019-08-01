/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "EnumTransformer.h"

#include "Creators.h"
#include "DexAsm.h"
#include "DexClass.h"
#include "EnumClinitAnalysis.h"
#include "EnumUpcastAnalysis.h"
#include "Mutators.h"
#include "OptData.h"
#include "Resolver.h"
#include "TypeReference.h"
#include "UsedVarsAnalysis.h"
#include "Walkers.h"

/**
 * We already get a set of candidate enums which are safe to be replaced with
 * Integer objects from EnumUpcastAnalysis, we do the transformation in the
 * EnumTransformer in following steps.
 * 1. Create an enum helper class LEnumUtils; with some helper methods and
 * singleton Integer fields, Integer f0, f1, f2 ....
 * 2. Update instructions.
 *  -- invoke-virtual LCandidateEnum;.ordinal()I =>
 *                    Ljava/lang/Integer;.intValue:()I
 *  -- invoke-static LCandidateEnum;.values():[LCandidateEnum; =>
 *                   LEnumUtils;.values(I)[Integer
 *  -- invoke-virtual LCandidateEnum;.compareTo:(Object)I =>
 *                    Ljava/lang/Integer;.compareTo:(Integer)I
 *  -- invoke-virtual LCandidateEnum;.equals:(Object)Z =>
 *                    Ljava/lang/Integer;.equals:(Object)Z
 *  -- sget-object LCandidateEnum;.f:LCandidateEnum; =>
 *                 LEnumUtils;.f?:Ljava/lang/Integer;
 *  -- invoke-virtual LCandidateEnum;.toString:()String or
 *     invoke-virtual LCandidateEnum;.name:()String =>
 *                    LCandidateEnum;.redex$OE$toString:(Integer)String
 *  -- invoke-virtual LCandidateEnum;.hashCode:()I =>
 *                    LCandidateEnum;.redex$OE$hashCode:(Integer)I
 *  -- invoke-static LCandidateEnum;.valueOf:(String)LCandidateEnum; =>
 *                   LCandidateEnum;.redex$OE$valueOf:(String)Integer
 *  We also make all non-true-virtual methods static (those are virtual methods
 *  that do not override and are not overridden by `Object`, `Enum`, or
 *  interface methods) and keep them in their original class while also changing
 *  their invocations to static.
 * 3. Clean the static fileds of candidate enums and update these enum classes
 * to inherit Object instead of Enum.
 * 4. Update specs of methods and fields based on name mangling.
 */

namespace {
using namespace optimize_enums;
using namespace dex_asm;
namespace mog = method_override_graph;
using EnumAttrMap =
    std::unordered_map<DexType*, std::unordered_map<const DexField*, EnumAttr>>;
namespace ptrs = local_pointers;

std::vector<EnumAttr> sort_enum_values(
    const std::unordered_map<const DexField*, EnumAttr>& enum_values) {
  std::map<uint32_t, const EnumAttr&> enums;
  for (auto& pair : enum_values) {
    enums.emplace(pair.second.ordinal, pair.second);
  }
  std::vector<EnumAttr> sorted;
  sorted.reserve(enums.size());
  for (auto& pair : enums) {
    sorted.emplace_back(pair.second);
  }
  return sorted;
}

/**
 * A structure holding the enum utils and constant values.
 */
struct EnumUtil {
  std::vector<DexFieldRef*> m_fields;

  // Store the needed helper methods for toString(), valueOf() and other
  // invocations at Code transformation phase, then implement these methods
  // later.
  ConcurrentSet<DexMethodRef*> m_substitute_methods;

  // Store non-true-virtual methods that will be made static later.
  ConcurrentSet<DexMethod*> m_non_true_virtual_methods;

  DexMethodRef* m_values_method_ref = nullptr;

  std::unique_ptr<const mog::Graph> m_method_override_graph;

  const Config& m_config;

  DexString* CLINIT_METHOD_STR = DexString::make_string("<clinit>");
  DexString* REDEX_TOSTRING = DexString::make_string("redex$OE$toString");
  DexString* REDEX_HASHCODE = DexString::make_string("redex$OE$hashCode");
  DexString* REDEX_STRING_VALUEOF =
      DexString::make_string("redex$OE$String_valueOf");
  DexString* REDEX_VALUEOF = DexString::make_string("redex$OE$valueOf");
  const DexString* INIT_METHOD_STR = DexString::make_string("<init>");
  const DexString* VALUES_METHOD_STR = DexString::make_string("values");
  const DexString* VALUEOF_METHOD_STR = DexString::make_string("valueOf");

  const DexString* VALUES_FIELD_STR = DexString::make_string("$VALUES");

  const DexType* ENUM_TYPE = get_enum_type();
  DexType* INT_TYPE = get_int_type();
  DexType* INTEGER_TYPE = get_integer_type();
  DexType* OBJECT_TYPE = get_object_type();
  DexType* STRING_TYPE = get_string_type();
  DexType* RTEXCEPTION_TYPE =
      DexType::make_type("Ljava/lang/RuntimeException;");
  DexType* ILLEGAL_ARG_EXCP_TYPE =
      DexType::make_type("Ljava/lang/IllegalArgumentException;");

  const DexMethodRef* ENUM_ORDINAL_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.ordinal:()I");
  const DexMethodRef* ENUM_EQUALS_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.equals:(Ljava/lang/Object;)Z");
  const DexMethodRef* ENUM_COMPARETO_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.compareTo:(Ljava/lang/Enum;)I");
  const DexMethodRef* ENUM_TOSTRING_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.toString:()Ljava/lang/String;");
  const DexMethodRef* ENUM_HASHCODE_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.hashCode:()I");
  const DexMethodRef* ENUM_NAME_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.name:()Ljava/lang/String;");
  const DexMethodRef* STRING_VALUEOF_METHOD = DexMethod::make_method(
      "Ljava/lang/String;.valueOf:(Ljava/lang/Object;)Ljava/lang/String;");
  const DexMethodRef* STRINGBUILDER_APPEND_OBJ_METHOD = DexMethod::make_method(
      "Ljava/lang/StringBuilder;.append:(Ljava/lang/Object;)Ljava/lang/"
      "StringBuilder;");
  DexMethodRef* STRING_HASHCODE_METHOD =
      DexMethod::make_method("Ljava/lang/String;.hashCode:()I");
  DexMethodRef* STRINGBUILDER_APPEND_STR_METHOD = DexMethod::make_method(
      "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/"
      "StringBuilder;");
  DexMethodRef* INTEGER_INTVALUE_METHOD =
      DexMethod::make_method("Ljava/lang/Integer;.intValue:()I");
  DexMethodRef* INTEGER_EQUALS_METHOD = DexMethod::make_method(
      "Ljava/lang/Integer;.equals:(Ljava/lang/Object;)Z");
  DexMethodRef* INTEGER_COMPARETO_METHOD = DexMethod::make_method(
      "Ljava/lang/Integer;.compareTo:(Ljava/lang/Integer;)I");
  DexMethodRef* INTEGER_VALUEOF_METHOD = DexMethod::make_method(
      "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;");
  DexMethodRef* RTEXCEPTION_CTOR_METHOD = DexMethod::make_method(
      "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V");
  DexMethodRef* ILLEGAL_ARG_CONSTRUCT_METHOD = DexMethod::make_method(
      "Ljava/lang/IllegalArgumentException;.<init>:(Ljava/lang/String;)V");
  DexMethodRef* STRING_EQ_METHOD =
      DexMethod::make_method("Ljava/lang/String;.equals:(Ljava/lang/Object;)Z");

  explicit EnumUtil(const Config& config,
                    std::unique_ptr<const mog::Graph> graph)
      : m_config(config) {
    m_method_override_graph = std::move(graph);
  }

  void create_util_class(DexStoresVector* stores, uint32_t fields_count) {
    DexClass* cls = make_enumutils_class(fields_count);
    auto& dexen = (*stores)[0].get_dexen()[0];
    dexen.push_back(cls);
  }

  /**
   * IF LCandidateEnum; is a candidate enum:
   *  LCandidateEnum; => Ljava/lang/Integer;
   *  [LCandidateEnum; => [Ljava/lang/Integer;
   *  [[LCandidateEnum; => [[Ljava/lang/Integer;
   *  ...
   * IF it is not a candidate enum, return nullptr.
   */
  DexType* try_convert_to_int_type(const EnumAttrMap& enums,
                                   DexType* type) const {
    uint32_t level = get_array_level(type);
    DexType* elem_type = type;
    if (level) {
      elem_type = get_array_type(type);
    }
    if (enums.count(elem_type)) {
      return level ? make_array_type(INTEGER_TYPE, level) : INTEGER_TYPE;
    }
    return nullptr;
  }

  /**
   * Return method ref to
   * LCandidateEnum;.redex$OE$String_valueOf:(Integer)String, a substitute for
   * String.valueOf:(Object) while the argument is a candidate enum object.
   * Store the method ref at the same time.
   *
   * The implemmentation of the substitute method depends on the substitute
   * method of LCandidateEnum;.toString:()String.
   */
  DexMethodRef* add_substitute_of_stringvalueof(DexType* enum_type) {
    auto tostring_meth = add_substitute_of_tostring(enum_type);
    auto method = DexMethod::make_method(enum_type, REDEX_STRING_VALUEOF,
                                         tostring_meth->get_proto());
    m_substitute_methods.insert(method);
    return method;
  }

  /**
   * Return method ref to LCandidateEnum;.redex$OE$valueOf(String):Integer, a
   * substitute for LCandidateEnum;.valueOf:(String)LCandidateEnum;.
   * Store the method ref at the same time.
   */
  DexMethodRef* add_substitute_of_valueof(DexType* enum_type) {
    auto proto = DexProto::make_proto(
        INTEGER_TYPE, DexTypeList::make_type_list({STRING_TYPE}));
    auto method = DexMethod::make_method(enum_type, REDEX_VALUEOF, proto);
    m_substitute_methods.insert(method);
    return method;
  }

  /**
   * Return method ref to LCandidateEnum;.redex$OE$toString:(Integer)String, a
   * substitute for LCandidateEnum;.toString:()String.
   * Store the method ref at the same time.
   */
  DexMethodRef* add_substitute_of_tostring(DexType* enum_type) {
    auto method = get_substitute_of_tostring(enum_type);
    m_substitute_methods.insert(method);
    return method;
  }

  /**
   * Return method ref to LCandidateEnum;.redex$OE$toString:(Integer)String
   */
  DexMethodRef* get_substitute_of_tostring(DexType* enum_type) {
    auto proto = DexProto::make_proto(
        STRING_TYPE, DexTypeList::make_type_list({INTEGER_TYPE}));
    auto method = DexMethod::make_method(enum_type, REDEX_TOSTRING, proto);
    return method;
  }

  /**
   * Returns a method ref to LCandidateEnum;.redex$OE$hashCode:(Integer)I, a
   * substitute for LCandidateEnum;.hashCode:()I.
   * Store the method ref at the same time.
   */
  DexMethodRef* add_substitute_of_hashcode(DexType* enum_type) {
    // `redex$OE$hashCode()` uses `redex$OE$toString()` so we better make sure
    // the method exists.
    add_substitute_of_tostring(enum_type);
    auto method = get_substitute_of_hashcode(enum_type);
    m_substitute_methods.insert(method);
    return method;
  }

  /**
   * Returns a method ref to LCandidateEnum;.redex$OE$hashCode:(Integer)I
   */
  DexMethodRef* get_substitute_of_hashcode(DexType* enum_type) {
    auto proto = DexProto::make_proto(
        INT_TYPE, DexTypeList::make_type_list({INTEGER_TYPE}));
    auto method = DexMethod::make_method(enum_type, REDEX_HASHCODE, proto);
    return method;
  }

 private:
  /**
   * Create a helper class for enums.
   */
  DexClass* make_enumutils_class(uint32_t fields_count) {
    std::string name = "Lredex/$EnumUtils;";
    DexType* type = DexType::get_type(name);
    while (type) {
      name.insert(name.size() - 1, "$u");
      type = DexType::get_type(name);
    }
    type = DexType::make_type(name.c_str());
    ClassCreator cc(type);
    cc.set_access(ACC_PUBLIC | ACC_FINAL);
    cc.set_super(get_object_type());
    DexClass* cls = cc.create();
    cls->rstate.set_generated();

    auto values_field = make_values_field(cls);
    auto clinit_method = make_clinit_method(cls, fields_count);
    auto clinit_code = clinit_method->get_code();
    m_fields.reserve(fields_count);
    for (uint32_t i = 0; i < fields_count; ++i) {
      m_fields.push_back(make_a_field(cls, i, clinit_code));
    }

    clinit_code->push_back(dasm(OPCODE_SPUT_OBJECT, values_field, {2_v}));
    clinit_code->push_back(dasm(OPCODE_RETURN_VOID));

    m_values_method_ref = make_values_method(cls, values_field);

    return cls;
  }

  /**
   * LEnumUtils;.$VALUES:[Ljava/lang/Integer;
   */
  DexFieldRef* make_values_field(DexClass* cls) {
    auto name = DexString::make_string("$VALUES");
    auto field = static_cast<DexField*>(DexField::make_field(
        cls->get_type(), name, make_array_type(INTEGER_TYPE)));
    field->make_concrete(
        ACC_PRIVATE | ACC_FINAL | ACC_STATIC,
        DexEncodedValue::zero_for_type(make_array_type(INTEGER_TYPE)));
    cls->add_field(field);
    return (DexFieldRef*)field;
  }

  /**
   * Create a static final Integer field and update <clinit> code.
   */
  DexFieldRef* make_a_field(DexClass* cls, uint32_t value, IRCode* code) {
    auto name = DexString::make_string("f" + std::to_string(value));
    auto field = static_cast<DexField*>(
        DexField::make_field(cls->get_type(), name, INTEGER_TYPE));
    field->make_concrete(ACC_PUBLIC | ACC_FINAL | ACC_STATIC,
                         DexEncodedValue::zero_for_type(INTEGER_TYPE));
    cls->add_field(field);
    code->push_back(dasm(OPCODE_CONST, {1_v, {LITERAL, value}}));
    code->push_back(dasm(OPCODE_INVOKE_STATIC, INTEGER_VALUEOF_METHOD, {1_v}));
    code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {0_v}));
    code->push_back(dasm(OPCODE_SPUT_OBJECT, field, {0_v}));
    code->push_back(dasm(OPCODE_APUT_OBJECT, {0_v, 2_v, 1_v}));
    return (DexFieldRef*)field;
  }

  /**
   * Make <clinit> method.
   */
  DexMethod* make_clinit_method(DexClass* cls, uint32_t fields_count) {
    auto proto =
        DexProto::make_proto(get_void_type(), DexTypeList::make_type_list({}));
    DexMethod* method = static_cast<DexMethod*>(
        DexMethod::make_method(cls->get_type(), CLINIT_METHOD_STR, proto));
    method->make_concrete(ACC_STATIC | ACC_CONSTRUCTOR, false);
    method->set_code(std::make_unique<IRCode>());
    cls->add_method(method);
    method->set_deobfuscated_name(show(method));
    auto code = method->get_code();

    // const v2, xx
    // new-array v2, v2, [Integer
    code->push_back(dasm(OPCODE_CONST, {2_v, {LITERAL, fields_count}}));
    code->push_back(
        dasm(OPCODE_NEW_ARRAY, make_array_type(INTEGER_TYPE), {2_v}));
    code->push_back(dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {2_v}));
    code->set_registers_size(3);
    return method;
  }

  /**
   * LEnumUtils;.values:(I)[Ljava/lang/Integer;
   */
  DexMethodRef* make_values_method(DexClass* cls, DexFieldRef* values_field) {
    DexString* name = DexString::make_string("values");
    DexProto* proto =
        DexProto::make_proto(make_array_type(INTEGER_TYPE),
                             DexTypeList::make_type_list({get_int_type()}));
    DexMethod* method = static_cast<DexMethod*>(
        DexMethod::make_method(cls->get_type(), name, proto));
    method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    method->set_code(std::make_unique<IRCode>());
    cls->add_method(method);
    method->set_deobfuscated_name(show(method));
    auto code = method->get_code();

    auto sub_array_method = DexMethod::make_method(
        "Ljava/util/Arrays;.copyOfRange:([Ljava/lang/Object;II)[Ljava/lang/"
        "Object;");

    code->push_back(dasm(IOPCODE_LOAD_PARAM, {1_v}));
    code->push_back(dasm(OPCODE_SGET_OBJECT, values_field));
    code->push_back(dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}));
    code->push_back(dasm(OPCODE_CONST, {2_v, 0_L}));
    code->push_back(
        dasm(OPCODE_INVOKE_STATIC, sub_array_method, {0_v, 2_v, 1_v}));
    code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {0_v}));
    code->push_back(
        dasm(OPCODE_CHECK_CAST, make_array_type(INTEGER_TYPE), {0_v}));
    code->push_back(dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}));
    code->push_back(dasm(OPCODE_RETURN_OBJECT, {0_v}));
    code->set_registers_size(3);

    return (DexMethodRef*)method;
  }
};

struct InsnReplacement {
  cfg::InstructionIterator original_insn;
  std::vector<IRInstruction*> replacements;

  InsnReplacement(cfg::ControlFlowGraph& cfg,
                  cfg::Block* block,
                  MethodItemEntry* mie,
                  IRInstruction* new_insn,
                  boost::optional<IROpcode> opcode = boost::none)
      : original_insn(block->to_cfg_instruction_iterator(*mie)),
        replacements{new_insn} {
    push_back_move_result(cfg, mie, opcode);
  }
  InsnReplacement(cfg::ControlFlowGraph& cfg,
                  cfg::Block* block,
                  MethodItemEntry* mie,
                  std::vector<IRInstruction*>& new_insns,
                  boost::optional<IROpcode> opcode = boost::none)
      : original_insn(block->to_cfg_instruction_iterator(*mie)),
        replacements(std::move(new_insns)) {
    push_back_move_result(cfg, mie, opcode);
  }

 private:
  /**
   * If the original instruction was paired with a `move-result`, create a new
   * one with the same destination register (and possibly the same opcode)
   * because the original one will be removed.
   */
  void push_back_move_result(cfg::ControlFlowGraph& cfg,
                             MethodItemEntry* mie,
                             boost::optional<IROpcode> opcode) {
    always_assert(mie->insn->has_move_result());
    auto move_insn_it = cfg.move_result_of(original_insn);
    if (!move_insn_it.is_end()) {
      auto& move_insn = move_insn_it.unwrap()->insn;
      replacements.push_back(dasm(opcode ? *opcode : move_insn->opcode(),
                                  {{VREG, move_insn->dest()}}));
    }
  }
};

/**
 * Code transformation for a method.
 */
class CodeTransformer final {
 public:
  CodeTransformer(const EnumAttrMap& enum_attrs,
                  EnumUtil* enum_util,
                  DexMethod* method)
      : m_enum_attrs(enum_attrs), m_enum_util(enum_util), m_method(method) {}

  void run() {
    optimize_enums::EnumTypeEnvironment start_env =
        optimize_enums::EnumFixpointIterator::gen_env(m_method);
    auto* code = m_method->get_code();
    code->build_cfg();
    auto& cfg = code->cfg();
    optimize_enums::EnumFixpointIterator engine(cfg, m_enum_util->m_config);
    engine.run(start_env);

    for (auto& block : cfg.blocks()) {
      optimize_enums::EnumTypeEnvironment env =
          engine.get_entry_state_at(block);
      for (auto it = block->begin(); it != block->end(); ++it) {
        if (it->type == MFLOW_OPCODE) {
          engine.analyze_instruction(it->insn, &env);
          update_instructions(env, cfg, block, &(*it));
        }
      }
    }

    // We could not insert invoke-kind instructions to editable cfg when we
    // iterate the cfg. If we're inside a try region, inserting invoke-kind will
    // split the block and insert a move-result in the new goto successor block,
    // thus invalidating iterators into the CFG. See the comment on the
    // insertion methods in ControlFlow.h for more details.
    for (const auto& info : m_replacements) {
      cfg.replace_insns(info.original_insn, info.replacements);
    }
    code->clear_cfg();
  }

 private:
  void update_instructions(const optimize_enums::EnumTypeEnvironment& env,
                           cfg::ControlFlowGraph& cfg,
                           cfg::Block* block,
                           MethodItemEntry* mie) {
    auto insn = mie->insn;
    switch (insn->opcode()) {
    case OPCODE_SGET_OBJECT:
      update_sget_object(env, cfg, block, mie);
      break;
    case OPCODE_INVOKE_VIRTUAL: {
      auto method = insn->get_method();
      if (signatures_match(method, m_enum_util->ENUM_ORDINAL_METHOD)) {
        update_invoke_virtual(env, cfg, block, mie,
                              m_enum_util->INTEGER_INTVALUE_METHOD);
      } else if (signatures_match(method, m_enum_util->ENUM_EQUALS_METHOD)) {
        update_invoke_virtual(env, cfg, block, mie,
                              m_enum_util->INTEGER_EQUALS_METHOD);
      } else if (signatures_match(method, m_enum_util->ENUM_COMPARETO_METHOD)) {
        update_invoke_virtual(env, cfg, block, mie,
                              m_enum_util->INTEGER_COMPARETO_METHOD);
      } else if (signatures_match(method, m_enum_util->ENUM_TOSTRING_METHOD) ||
                 signatures_match(method, m_enum_util->ENUM_NAME_METHOD)) {
        update_invoke_tostring(env, cfg, block, mie);
      } else if (signatures_match(method, m_enum_util->ENUM_HASHCODE_METHOD)) {
        update_invoke_hashcode(env, cfg, block, mie);
      } else if (method == m_enum_util->STRINGBUILDER_APPEND_OBJ_METHOD) {
        update_invoke_stringbuilder_append(env, cfg, block, mie);
      } else {
        auto resolved_method = resolve_method(method, MethodSearch::Virtual);
        // Matches all non-true-virtual `SubEnum` methods (virtual methods that
        // do not override and are not overridden by any `Enum`, `Object`, or
        // interface methods).
        if (m_enum_util->m_config.candidate_enums.count(method->get_class()) &&
            mog::get_true_virtuals(*m_enum_util->m_method_override_graph,
                                   resolved_method,
                                   /*include_interfaces=*/true)
                .empty()) {
          update_invoke_user_method(cfg, block, mie, resolved_method);
        }
      }
    } break;
    case OPCODE_INVOKE_STATIC: {
      auto method = insn->get_method();
      if (method == m_enum_util->STRING_VALUEOF_METHOD) {
        update_invoke_string_valueof(env, cfg, block, mie);
      } else if (is_enum_values(method)) {
        update_invoke_values(env, cfg, block, mie);
      } else if (is_enum_valueof(method)) {
        update_invoke_valueof(env, cfg, block, mie);
      }
    } break;
    case OPCODE_NEW_ARRAY: {
      auto array_type = insn->get_type();
      auto new_type = try_convert_to_int_type(array_type);
      if (new_type) {
        insn->set_type(new_type);
      }
    } break;
    case OPCODE_CHECK_CAST: {
      auto type = insn->get_type();
      if (try_convert_to_int_type(type)) {
        DexType* candidate_type =
            extract_candidate_enum_type(env.get(insn->src(0)));
        always_assert(candidate_type == type);
        m_replacements.push_back(
            InsnReplacement(cfg, block, mie,
                            dasm(OPCODE_CHECK_CAST, m_enum_util->INTEGER_TYPE,
                                 {{VREG, insn->src(0)}})));
      } else if (type == m_enum_util->ENUM_TYPE) {
        always_assert(!extract_candidate_enum_type(env.get(insn->src(0))));
      }
    } break;
    default: {
      if (insn->has_type()) {
        auto type = insn->get_type();
        always_assert_log(try_convert_to_int_type(type) == nullptr,
                          "Unhandled type in %s method %s\n", SHOW(insn),
                          SHOW(m_method));
      }
    } break;
    }
  }

  /**
   * If the field is a candidate enum field,
   * sget-object LCandidateEnum;.f:LCandidateEnum; =>
   *   sget-object LEnumUtils;.f?:Integer
   */
  void update_sget_object(const optimize_enums::EnumTypeEnvironment& env,
                          cfg::ControlFlowGraph& cfg,
                          cfg::Block* block,
                          MethodItemEntry* mie) {
    auto insn = mie->insn;
    auto field = static_cast<DexField*>(insn->get_field());
    if (!m_enum_attrs.count(field->get_type())) {
      return;
    }
    auto& enum_attrs = m_enum_attrs.at(field->get_type());
    if (!enum_attrs.count(field)) {
      return;
    }
    auto ord = enum_attrs.at(field).ordinal;
    auto new_field = m_enum_util->m_fields[ord];
    auto new_insn = dasm(OPCODE_SGET_OBJECT, new_field);
    m_replacements.push_back(InsnReplacement(cfg, block, mie, new_insn));
  }

  /**
   * If LCandidateEnum; is a candidate enum class,
   * invoke-static LCandidateEnum;.values:()[LCandidateEnum; =>
   *   const vn, xxx
   *   invoke-static vn LEnumUtils;.values:(I)[Integer
   */
  void update_invoke_values(const optimize_enums::EnumTypeEnvironment&,
                            cfg::ControlFlowGraph& cfg,
                            cfg::Block* block,
                            MethodItemEntry* mie) {
    auto insn = mie->insn;
    auto method = insn->get_method();
    auto container = method->get_class();
    auto attrs_it = m_enum_attrs.find(container);
    if (attrs_it != m_enum_attrs.end()) {
      auto reg = allocate_temp();
      auto cls = type_class(container);
      uint64_t enum_size = attrs_it->second.size();
      always_assert(enum_size);
      std::vector<IRInstruction*> new_insns;
      new_insns.push_back(
          dasm(OPCODE_CONST, {{VREG, reg}, {LITERAL, enum_size}}));
      new_insns.push_back(dasm(OPCODE_INVOKE_STATIC,
                               m_enum_util->m_values_method_ref,
                               {{VREG, reg}}));
      m_replacements.push_back(InsnReplacement(cfg, block, mie, new_insns));
    }
  }

  /**
   * If LCandidateEnum; is a candidate enum class,
   * invoke-static v0 LCandidateEnum;.valueOf:(String)LCandidateEnum; =>
   *   invoke-static v0 LCandidateEnum;.redex$OE$valueOf:(String)Integer
   */
  void update_invoke_valueof(const optimize_enums::EnumTypeEnvironment&,
                             cfg::ControlFlowGraph& cfg,
                             cfg::Block* block,
                             MethodItemEntry* mie) {
    auto insn = mie->insn;
    auto container = insn->get_method()->get_class();
    if (!m_enum_attrs.count(container)) {
      return;
    }
    auto valueof_method = m_enum_util->add_substitute_of_valueof(container);
    auto reg = insn->src(0);
    m_replacements.push_back(InsnReplacement(
        cfg, block, mie,
        dasm(OPCODE_INVOKE_STATIC, valueof_method, {{VREG, reg}})));
  }

  /**
   * If v0 is a candidate enum,
   * invoke-virtual v0 LCandidateEnum;.toString:()Ljava/lang/String; or
   * invoke-virtual v0 LCandidateEnum;.name:()Ljava/lang/String; =>
   *    invoke-static v0 LCandidateEnum;.redex$OE$toString:(Integer)String
   * Since we never optimize enums that override toString method,
   * LCandidateEnum;.toString:()String and LCandidateEnum;.name:()String are the
   * same when LCandidateEnum; is a candidate enum.
   */
  void update_invoke_tostring(const optimize_enums::EnumTypeEnvironment& env,
                              cfg::ControlFlowGraph& cfg,
                              cfg::Block* block,
                              MethodItemEntry* mie) {
    auto insn = mie->insn;
    auto container = insn->get_method()->get_class();
    if (container != m_enum_util->OBJECT_TYPE &&
        container != m_enum_util->ENUM_TYPE && !m_enum_attrs.count(container)) {
      return;
    }
    DexType* candidate_type =
        extract_candidate_enum_type(env.get(insn->src(0)));
    if (m_enum_attrs.count(container)) {
      always_assert(candidate_type == nullptr || candidate_type == container);
      candidate_type = container;
    } else if (!candidate_type) {
      return;
    }
    auto helper_method =
        m_enum_util->add_substitute_of_tostring(candidate_type);
    auto reg = insn->src(0);
    m_replacements.push_back(InsnReplacement(
        cfg, block, mie,
        dasm(OPCODE_INVOKE_STATIC, helper_method, {{VREG, reg}})));
  }

  /**
   * If v0 is a candidate enum,
   * invoke-virtual v0 LCandidateEnum;.hashCode:()I =>
   *    invoke-static v0 LCandidateEnum;.redex$OE$hashCode:(Integer)I
   */
  void update_invoke_hashcode(const optimize_enums::EnumTypeEnvironment& env,
                              cfg::ControlFlowGraph& cfg,
                              cfg::Block* block,
                              MethodItemEntry* mie) {
    auto insn = mie->insn;
    auto container = insn->get_method()->get_class();
    if (container != m_enum_util->OBJECT_TYPE &&
        container != m_enum_util->ENUM_TYPE &&
        m_enum_attrs.count(container) == 0) {
      return;
    }
    auto src_reg = insn->src(0);
    DexType* candidate_type = extract_candidate_enum_type(env.get(src_reg));
    if (m_enum_attrs.count(container)) {
      always_assert(candidate_type == nullptr || candidate_type == container);
      candidate_type = container;
    } else if (!candidate_type) {
      return;
    }
    auto helper_method =
        m_enum_util->add_substitute_of_hashcode(candidate_type);
    m_replacements.push_back(InsnReplacement(
        cfg, block, mie,
        dasm(OPCODE_INVOKE_STATIC, helper_method, {{VREG, src_reg}})));
  }

  /**
   * If v0 is a candidate enum object,
   * invoke-static v0 LString;.valueOf:(LObject;)LString;
   * =>
   *   invoke-static v0 LCandidateEnum;.redex$OE$String_valueOf:(Integer)String
   */
  void update_invoke_string_valueof(
      const optimize_enums::EnumTypeEnvironment& env,
      cfg::ControlFlowGraph& cfg,
      cfg::Block* block,
      MethodItemEntry* mie) {
    auto insn = mie->insn;
    DexType* candidate_type =
        extract_candidate_enum_type(env.get(insn->src(0)));
    if (candidate_type == nullptr) {
      return;
    }
    DexMethodRef* string_valueof_meth =
        m_enum_util->add_substitute_of_stringvalueof(candidate_type);
    m_replacements.push_back(
        InsnReplacement(cfg, block, mie,
                        dasm(OPCODE_INVOKE_STATIC, string_valueof_meth,
                             {{VREG, insn->src(0)}})));
  }

  /**
   * If v1 is a candidate enum,
   * invoke-virtual v0 v1 LStringBuilder;.append(Object):LStringBuilder;
   * =>
   *   invoke-static v1 LCandidateEnum;.redex$OE$String_valueOf:(Integer)String
   *   move-result-object vn
   *   invoke-virtual v0 vn LStringBuilder;.append:(String)LStringBuilder;
   */
  void update_invoke_stringbuilder_append(
      const optimize_enums::EnumTypeEnvironment& env,
      cfg::ControlFlowGraph& cfg,
      cfg::Block* block,
      MethodItemEntry* mie) {
    auto insn = mie->insn;
    DexType* candidate_type =
        extract_candidate_enum_type(env.get(insn->src(1)));
    if (candidate_type == nullptr) {
      return;
    }
    DexMethodRef* string_valueof_meth =
        m_enum_util->add_substitute_of_stringvalueof(candidate_type);
    auto reg0 = insn->src(0);
    auto reg1 = insn->src(1);
    auto str_reg = allocate_temp();
    std::vector<IRInstruction*> new_insns{
        dasm(OPCODE_INVOKE_STATIC, string_valueof_meth, {{VREG, reg1}}),
        dasm(OPCODE_MOVE_RESULT_OBJECT, {{VREG, str_reg}}),
        dasm(OPCODE_INVOKE_VIRTUAL,
             m_enum_util->STRINGBUILDER_APPEND_STR_METHOD,
             {{VREG, reg0}, {VREG, str_reg}})};
    m_replacements.push_back(InsnReplacement(cfg, block, mie, new_insns));
  }

  /**
   * If v0 is a candidate enum,
   * invoke-virtual v0 LCandidateEnum;.ordinal:()I =>
   * invoke-virtual v0 Integer.intValue()I,
   *
   * invoke-virtual v0, v1 LCandidateEnum;.equals:(Ljava/lang/Object;)Z =>
   * invoke-virtual v0, v1 Integer.equals(Ljava/lang/Object;)Z,
   *
   * invoke-virtual v0, v1 LCandidateEnum;.compareTo:(Ljava/lang/Object;)I =>
   * invoke-virtual v0, v1 Integer.compareTo(Ljava/lang/Integer;)I
   */
  void update_invoke_virtual(const optimize_enums::EnumTypeEnvironment& env,
                             cfg::ControlFlowGraph& cfg,
                             cfg::Block* block,
                             MethodItemEntry* mie,
                             DexMethodRef* integer_meth) {
    auto insn = mie->insn;
    auto container = insn->get_method()->get_class();
    if (container != m_enum_util->OBJECT_TYPE &&
        container != m_enum_util->ENUM_TYPE && !m_enum_attrs.count(container)) {
      return;
    }
    auto this_types = env.get(insn->src(0));
    DexType* candidate_type = extract_candidate_enum_type(this_types);
    if (m_enum_attrs.count(container)) {
      always_assert(candidate_type == nullptr || candidate_type == container);
    } else if (!candidate_type) {
      return;
    }
    auto new_insn = new IRInstruction(OPCODE_INVOKE_VIRTUAL);
    new_insn->set_method(integer_meth)->set_arg_word_count(insn->srcs_size());
    for (size_t id = 0; id < insn->srcs_size(); ++id) {
      new_insn->set_src(id, insn->src(id));
    }
    m_replacements.push_back(InsnReplacement(cfg, block, mie, new_insn));
  }

  void update_invoke_user_method(cfg::ControlFlowGraph& cfg,
                                 cfg::Block* block,
                                 MethodItemEntry* mie,
                                 DexMethod* method) {
    m_enum_util->m_non_true_virtual_methods.insert(method);
    auto new_insn =
        (new IRInstruction(*mie->insn))->set_opcode(OPCODE_INVOKE_STATIC);
    m_replacements.push_back(InsnReplacement(cfg, block, mie, new_insn));
  }

  /**
   * Return nullptr if the types contain none of the candidate enums,
   * return the candidate type if types only contain one candidate enum and do
   * not contain other types,
   * or assertion failure when the types are mixed.
   */
  DexType* extract_candidate_enum_type(const EnumTypes& types) {
    auto type_set = types.elements();
    if (std::all_of(type_set.begin(), type_set.end(),
                    [this](DexType* t) { return !m_enum_attrs.count(t); })) {
      return nullptr;
    }
    DexType* ret = nullptr;

    for (auto t : type_set) {
      if (m_enum_attrs.count(t)) {
        always_assert_log(ret == nullptr,
                          "Multiple candidate enums %s and %s\n", SHOW(t),
                          SHOW(ret));
        ret = t;
      }
    }
    return ret;
  }

  DexType* try_convert_to_int_type(DexType* type) {
    return m_enum_util->try_convert_to_int_type(m_enum_attrs, type);
  }

  inline uint16_t allocate_temp() {
    return m_method->get_code()->cfg().allocate_temp();
  }

  const EnumAttrMap& m_enum_attrs;
  EnumUtil* m_enum_util;
  DexMethod* m_method;
  std::vector<InsnReplacement> m_replacements;
};

/**
 * Transform enum usages in the stores.
 */
class EnumTransformer final {
 public:
  /**
   * EnumTransformer constructor. Analyze <clinit> of candidate enums.
   */
  EnumTransformer(const Config& config,
                  DexStoresVector* stores,
                  std::unique_ptr<const mog::Graph> graph)
      : m_stores(*stores), m_int_objs(0) {
    m_enum_util = std::make_unique<EnumUtil>(config, std::move(graph));
    for (auto it = config.candidate_enums.begin();
         it != config.candidate_enums.end();
         ++it) {
      auto enum_cls = type_class(*it);
      auto enum_attrs = optimize_enums::analyze_enum_clinit(enum_cls);
      if (enum_attrs.empty()) {
        TRACE(ENUM, 2, "\tCannot analyze enum %s : ord %lu sfields %lu",
              SHOW(enum_cls), enum_attrs.size(),
              enum_cls->get_sfields().size());
        continue;
      } else if (enum_attrs.size() > config.max_enum_size) {
        TRACE(ENUM, 2, "\tSkip %s %lu values", SHOW(enum_cls),
              enum_attrs.size());
      } else {
        m_int_objs = std::max<uint32_t>(m_int_objs, enum_attrs.size());
        m_enum_objs += enum_attrs.size();
        m_enum_attrs.emplace(*it, enum_attrs);
        clean_generated_methods_fields(enum_cls);
        opt_metadata::log_opt(ENUM_OPTIMIZED, enum_cls);
      }
    }
    m_enum_util->create_util_class(stores, m_int_objs);
  }

  EnumTransformer(const EnumTransformer&) = delete;

  void run() {
    auto scope = build_class_scope(m_stores);
    // Update all the instructions.
    walk::parallel::code(
        scope,
        [&](DexMethod* method) {
          if (m_enum_attrs.count(method->get_class()) &&
              is_generated_enum_method(method)) {
            return false;
          }
          std::vector<DexType*> types;
          method->get_proto()->gather_types(types);
          method->gather_types(types);
          return std::any_of(types.begin(), types.end(), [this](DexType* type) {
            return (bool)try_convert_to_int_type(type);
          });
        },
        [&](DexMethod* method, IRCode& code) {
          CodeTransformer code_updater(m_enum_attrs, m_enum_util.get(), method);
          code_updater.run();
        });
    create_substitute_methods(m_enum_util->m_substitute_methods);
    for (DexMethod* vmethod : m_enum_util->m_non_true_virtual_methods) {
      mutators::make_static(vmethod);
    }
    post_update_enum_classes(scope);
    // Update all methods and fields references by replacing the candidate enum
    // types with Integer type.
    std::unordered_map<DexType*, DexType*> type_mapping;
    for (auto& pair : m_enum_attrs) {
      type_mapping[pair.first] = m_enum_util->INTEGER_TYPE;
    }
    type_reference::TypeRefUpdater updater(type_mapping);
    updater.update_methods_fields(scope);
    sanity_check(scope);
  }

  uint32_t get_int_objs_count() { return m_int_objs; }
  uint32_t get_enum_objs_count() { return m_enum_objs; }

 private:
  /**
   * Go through all instructions and check that all the methods, fields, and
   * types they reference actually exist.
   */
  void sanity_check(Scope& scope) {
    walk::parallel::code(scope, [this](DexMethod* method, IRCode& code) {
      for (auto& mie : InstructionIterable(code)) {
        auto insn = mie.insn;
        if (insn->has_method()) {
          auto method_ref = insn->get_method();
          auto container = method_ref->get_class();
          if (m_enum_attrs.count(container)) {
            always_assert_log(method_ref->is_def(), "Invalid insn %s in %s\n",
                              SHOW(insn), SHOW(method));
          }
        } else if (insn->has_field()) {
          auto field_ref = insn->get_field();
          auto container = field_ref->get_class();
          if (m_enum_attrs.count(container)) {
            always_assert_log(field_ref->is_def(), "Invalid insn %s in %s\n",
                              SHOW(insn), SHOW(method));
          }
        } else if (insn->has_type()) {
          auto type_ref = insn->get_type();
          always_assert_log(!m_enum_attrs.count(type_ref),
                            "Invalid insn %s in %s\n", SHOW(insn),
                            SHOW(method));
        }
      }
    });
  }

  void create_substitute_methods(const ConcurrentSet<DexMethodRef*>& methods) {
    for (auto ref : methods) {
      if (ref->get_name() == m_enum_util->REDEX_TOSTRING) {
        create_tostring_method(ref);
      } else if (ref->get_name() == m_enum_util->REDEX_HASHCODE) {
        create_hashcode_method(ref);
      } else if (ref->get_name() == m_enum_util->REDEX_VALUEOF) {
        create_valueof_method(ref);
      } else if (ref->get_name() == m_enum_util->REDEX_STRING_VALUEOF) {
        create_stringvalueof_method(ref);
      }
    }
  }

  /**
   * Substitute for String.valueOf(Object obj).
   *
   * public static String redex$OE$String_valueOf(Integer obj) {
   *   if (obj == null) {
   *     return "null";
   *   }
   *   return redex$OE$toString(obj);
   * }
   */
  void create_stringvalueof_method(DexMethodRef* ref) {
    MethodCreator mc(ref, ACC_STATIC | ACC_PUBLIC);
    auto method = mc.create();
    auto cls = type_class(ref->get_class());
    cls->add_method(method);
    auto code = method->get_code();
    code->build_cfg();
    auto& cfg = code->cfg();
    auto entry = cfg.entry_block();
    auto return_null_block = cfg.create_block();
    return_null_block->push_back(
        {dasm(OPCODE_CONST_STRING, DexString::make_string("null")),
         dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {1_v}),
         dasm(OPCODE_RETURN_OBJECT, {1_v})});
    auto obj_tostring_block = cfg.create_block();
    {
      auto tostring_meth =
          m_enum_util->get_substitute_of_tostring(ref->get_class());
      obj_tostring_block->push_back(
          {dasm(OPCODE_INVOKE_STATIC, tostring_meth, {0_v}),
           dasm(OPCODE_MOVE_RESULT_OBJECT, {1_v}),
           dasm(OPCODE_RETURN_OBJECT, {1_v})});
    }
    cfg.create_branch(entry, dasm(OPCODE_IF_EQZ, {0_v}), obj_tostring_block,
                      return_null_block);
    cfg.recompute_registers_size();
    code->clear_cfg();
  }

  /**
   * Substitute for LCandidateEnum;.valueOf(String s)
   *
   * public static Integer redex$OE$valueOf(String s) {
   *   if (s == "xxx") {
   *     return f0;
   *   } else if (s == "y") {
   *     return f1;
   *   } ...
   *   } else {
   *     throw new IllegalArgumentException(s);
   *   }
   * }
   *
   * Note that the string of the exception is shortened.
   */
  void create_valueof_method(DexMethodRef* ref) {
    MethodCreator mc(ref, ACC_STATIC | ACC_PUBLIC);
    auto method = mc.create();
    auto cls = type_class(ref->get_class());
    cls->add_method(method);
    auto code = method->get_code();
    code->build_cfg();
    auto& cfg = code->cfg();
    auto prev_block = cfg.entry_block();
    auto& enum_attrs = m_enum_attrs.at(ref->get_class());
    auto sorted_values = sort_enum_values(enum_attrs);
    for (auto& value : sorted_values) {
      prev_block->push_back(
          {dasm(OPCODE_CONST_STRING, const_cast<DexString*>(value.name)),
           dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {1_v}),
           dasm(OPCODE_INVOKE_VIRTUAL, m_enum_util->STRING_EQ_METHOD,
                {0_v, 1_v}),
           dasm(OPCODE_MOVE_RESULT, {3_v})});

      auto equal_block = cfg.create_block();
      {
        auto obj_field = m_enum_util->m_fields[value.ordinal];
        equal_block->push_back({dasm(OPCODE_SGET_OBJECT, obj_field),
                                dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {2_v}),
                                dasm(OPCODE_RETURN_OBJECT, {2_v})});
      }
      auto ne_block = cfg.create_block();
      cfg.create_branch(prev_block, dasm(OPCODE_IF_EQZ, {3_v}), equal_block,
                        ne_block);
      prev_block = ne_block;
    }
    prev_block->push_back(
        {dasm(OPCODE_NEW_INSTANCE, m_enum_util->ILLEGAL_ARG_EXCP_TYPE, {}),
         dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {1_v}),
         dasm(OPCODE_INVOKE_DIRECT,
              m_enum_util->ILLEGAL_ARG_CONSTRUCT_METHOD,
              {1_v, 0_v}),
         dasm(OPCODE_THROW, {1_v})});
    cfg.recompute_registers_size();
    code->clear_cfg();
  }

  /**
   * Substitute for LCandidateEnum;.toString()
   *
   * public static String redex$OE$toString(Integer obj) {
   *   switch(obj.intValue()) {
   *     case 0 : ...;
   *     case 1 : ...;
   *     ...
   *   }
   * }
   */
  void create_tostring_method(DexMethodRef* ref) {
    MethodCreator mc(ref, ACC_STATIC | ACC_PUBLIC);
    auto method = mc.create();
    auto cls = type_class(ref->get_class());
    cls->add_method(method);
    auto code = method->get_code();
    code->build_cfg();
    auto& cfg = code->cfg();
    auto entry = cfg.entry_block();
    entry->push_back({dasm(OPCODE_INVOKE_VIRTUAL,
                           m_enum_util->INTEGER_INTVALUE_METHOD, {0_v}),
                      dasm(OPCODE_MOVE_RESULT, {0_v})});

    auto& enum_attrs = m_enum_attrs.at(ref->get_class());
    auto sorted_values = sort_enum_values(enum_attrs);
    std::vector<std::pair<int32_t, cfg::Block*>> cases;
    for (auto& value : sorted_values) {
      auto block = cfg.create_block();
      cases.emplace_back(value.ordinal, block);
      block->push_back(
          {dasm(OPCODE_CONST_STRING, const_cast<DexString*>(value.name)),
           dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {1_v}),
           dasm(OPCODE_RETURN_OBJECT, {1_v})});
    }
    // This goto edge should never be taken, but we need a goto edge because the
    // switch is not a valid way to end a method. A switch cannot end a block
    // because the on-device dex verifier is unable to prove if the switch is
    // exhaustive.
    //
    // Arbitrarily choose the first case block.
    cfg.create_branch(entry, dasm(OPCODE_SWITCH, {0_v}), cases.front().second,
                      cases);
    cfg.recompute_registers_size();
    code->clear_cfg();
  }

  /**
   * Substitute for LCandidateEnum:.hashCode()
   *
   * Since `Enum.hashCode()` is not in the Java spec so that different JVMs
   * may have different implementations and since hashcodes are usually
   * only used as keys to hash maps we can choose one implementation.
   * https://android.googlesource.com/platform/libcore/+/9edf43dfcc35c761d97eb9156ac4254152ddbc55/libdvm/src/main/java/java/lang/Enum.java#118
   *
   *
   * public static int redex$OE$hashCode(Integer obj) {
   *   String name = redex$OE$toString(obj);
   *   return obj.intValue() + name.hashCode();
   * }
   */
  void create_hashcode_method(DexMethodRef* ref) {
    MethodCreator mc(ref, ACC_STATIC | ACC_PUBLIC);
    auto method = mc.create();
    auto cls = type_class(ref->get_class());
    cls->add_method(method);
    auto code = method->get_code();
    code->build_cfg();
    auto& cfg = code->cfg();
    auto entry = cfg.entry_block();
    auto tostring_method =
        m_enum_util->get_substitute_of_tostring(ref->get_class());
    entry->push_back({
        dasm(OPCODE_INVOKE_STATIC, tostring_method, {0_v}),
        dasm(OPCODE_MOVE_RESULT_OBJECT, {1_v}),
        dasm(OPCODE_INVOKE_VIRTUAL, m_enum_util->STRING_HASHCODE_METHOD, {1_v}),
        dasm(OPCODE_MOVE_RESULT, {1_v}),
        dasm(OPCODE_INVOKE_VIRTUAL, m_enum_util->INTEGER_INTVALUE_METHOD,
             {0_v}),
        dasm(OPCODE_MOVE_RESULT, {2_v}),
        dasm(OPCODE_ADD_INT, {1_v, 1_v, 2_v}),
        dasm(OPCODE_RETURN, {1_v}),
    });
    cfg.recompute_registers_size();
    code->clear_cfg();
  }

  /**
   * 1. Erase the enum instance fields and synthetic array field which is
   * usually `$VALUES`.
   * 2. Delete <init>, values() and valueOf(String) methods, and delete
   * instructions that construct these fields from <clinit>.
   */
  void clean_generated_methods_fields(DexClass* enum_cls) {
    auto& sfields = enum_cls->get_sfields();
    auto& attrs = m_enum_attrs.at(enum_cls->get_type());
    auto synth_field_access = synth_access();
    DexField* values_field = nullptr;

    for (auto fit = sfields.begin(); fit != sfields.end();) {
      auto field = *fit;
      if (attrs.count(field)) {
        fit = sfields.erase(fit);
      } else if (check_required_access_flags(synth_field_access,
                                             field->get_access())) {
        always_assert(!values_field);
        values_field = field;
        fit = sfields.erase(fit);
      } else {
        ++fit;
      }
    }

    always_assert(values_field);
    auto& dmethods = enum_cls->get_dmethods();
    // Delete <init>, values() and valueOf(String) methods, and clean <clinit>.
    for (auto mit = dmethods.begin(); mit != dmethods.end();) {
      auto method = *mit;
      if (is_clinit(method)) {
        clean_clinit(attrs, enum_cls, method, values_field);
        if (empty(method->get_code())) {
          mit = dmethods.erase(mit);
        } else {
          ++mit;
        }
      } else if (is_generated_enum_method(method)) {
        mit = dmethods.erase(mit);
      } else {
        ++mit;
      }
    }
  }

  /**
   * Erase the put instructions that write enum values and synthetic $VALUES
   * array, then erase the dead instructions.
   */
  void clean_clinit(const AttrMap& attrs,
                    DexClass* enum_cls,
                    DexMethod* clinit,
                    DexField* values_field) {
    auto code = clinit->get_code();
    auto ctors = enum_cls->get_ctors();
    always_assert(ctors.size() == 1);
    auto ctor = ctors[0];
    side_effects::InvokeToSummaryMap summaries;

    for (auto it = code->begin(); it != code->end();) {
      if (it->type != MFLOW_OPCODE) {
        ++it;
        continue;
      }
      auto insn = it->insn;
      if (is_sput(insn->opcode())) {
        auto field = resolve_field(insn->get_field());
        if (field && (attrs.count(field) || field == values_field)) {
          it = code->erase(it);
          continue;
        }
      } else if (is_invoke_direct(insn->opcode()) &&
                 insn->get_method() == ctor) {
        summaries.emplace(insn, side_effects::Summary());
      }
      ++it;
    }

    code->build_cfg(/* editable */ false);
    auto& cfg = code->cfg();
    cfg.calculate_exit_block();
    ptrs::FixpointIterator fp_iter(cfg);
    fp_iter.run(ptrs::Environment());
    used_vars::FixpointIterator uv_fpiter(fp_iter, summaries, cfg);
    uv_fpiter.run(used_vars::UsedVarsSet());
    auto dead_instructions = used_vars::get_dead_instructions(*code, uv_fpiter);
    code->clear_cfg();
    for (auto insn : dead_instructions) {
      code->remove_opcode(insn);
    }
  }

  /**
   * Only use for <clinit> code.
   */
  bool empty(IRCode* code) {
    auto iterable = InstructionIterable(code);
    auto begin = iterable.begin();
    return is_return_void(begin->insn->opcode());
  }

  /**
   * Whether a method is <init>, values() or valueOf(String).
   */
  bool is_generated_enum_method(DexMethodRef* method) {
    auto name = method->get_name();
    return name == m_enum_util->INIT_METHOD_STR || is_enum_values(method) ||
           is_enum_valueof(method);
  }

  /**
   * Change candidates' superclass from Enum to Object.
   */
  void post_update_enum_classes(Scope& scope) {
    for (auto cls : scope) {
      if (!m_enum_attrs.count(cls->get_type())) {
        continue;
      }
      always_assert_log(cls->get_super_class() == m_enum_util->ENUM_TYPE,
                        "%s super %s\n",
                        SHOW(cls),
                        SHOW(cls->get_super_class()));
      cls->set_super_class(m_enum_util->OBJECT_TYPE);
      cls->set_access(cls->get_access() & ~ACC_ENUM);
    }
  }

  DexType* try_convert_to_int_type(DexType* type) {
    return m_enum_util->try_convert_to_int_type(m_enum_attrs, type);
  }

  DexStoresVector& m_stores;
  uint32_t m_int_objs{0}; // Generated Integer objects.
  uint32_t m_enum_objs{0}; // Eliminated Enum objects.
  EnumAttrMap m_enum_attrs;
  std::unique_ptr<EnumUtil> m_enum_util;
};
} // namespace

namespace optimize_enums {
/**
 * Transform enums to Integer objects, return the total number of eliminated
 * enum objects.
 */
int transform_enums(const Config& config,
                    DexStoresVector* stores,
                    std::unique_ptr<const mog::Graph> graph,
                    size_t* num_int_objs) {
  if (!config.candidate_enums.size()) {
    return 0;
  }

  EnumTransformer transformer(config, stores, std::move(graph));
  transformer.run();
  *num_int_objs = transformer.get_int_objs_count();
  return transformer.get_enum_objs_count();
}
} // namespace optimize_enums
