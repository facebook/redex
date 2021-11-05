/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "EnumTransformer.h"

#include "Creators.h"
#include "DexAsm.h"
#include "DexClass.h"
#include "EnumUpcastAnalysis.h"
#include "Mutators.h"
#include "OptData.h"
#include "Resolver.h"
#include "Show.h"
#include "StlUtil.h"
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
 *       or construct a new integer if the enum is allowed to be optimized
 *       unsafely.
 *  -- invoke-virtual LCandidateEnum;.name:()String =>
 *                    LCandidateEnum;.redex$OE$name:(Integer)String
 *  -- invoke-virtual LCandidateEnum;.hashCode:()I =>
 *                    LCandidateEnum;.redex$OE$hashCode:(Integer)I
 *  -- invoke-static LCandidateEnum;.valueOf:(String)LCandidateEnum; =>
 *                   LCandidateEnum;.redex$OE$valueOf:(String)Integer
 *
 *  If CandidateEnum.toString() overrides Enum.toString()
 *  -- invoke-virtual LCandidateEnum;.toString:()String =>
 *                    LCandidateEnum;.toString$REDEX$YCYa1bLthVk:(Integer)String
 *  otherwise
 *  -- invoke-virtual LCandidateEnum;.toString:()String =>
 *                    LCandidateEnum;.redex$OE$name:(Integer)String
 *
 *  We also make all virtual methods and instance direct methods to be static
 * and keep them in their original class while also changing their invocations
 * to static.
 * 3. Clean up the static fields of candidate enums and update these enum
 * classes to inherit java.lang.Object instead of java.lang.Enum.
 * 4. Update specs of methods and fields based on name mangling.
 */

namespace {
using namespace optimize_enums;
using namespace dex_asm;
using EnumAttributeMap = std::unordered_map<DexType*, EnumAttributes>;
namespace ptrs = local_pointers;

/**
 * A structure holding the enum utils and constant values.
 */
struct EnumUtil {
  std::vector<DexFieldRef*> m_fields;

  // Store the needed helper methods for toString(), valueOf() and other
  // invocations at Code transformation phase, then implement these methods
  // later.
  ConcurrentSet<DexMethodRef*> m_substitute_methods;

  // Store virtual and direct methods of candidate enums that will be
  // made static later.
  ConcurrentSet<DexMethod*> m_instance_methods;

  // Store methods for getting instance fields to be generated later.
  ConcurrentMap<DexFieldRef*, DexMethodRef*> m_get_instance_field_methods;

  DexMethodRef* m_values_method_ref = nullptr;

  const Config& m_config;

  const DexString* CLINIT_METHOD_STR = DexString::make_string("<clinit>");
  const DexString* REDEX_NAME = DexString::make_string("redex$OE$name");
  const DexString* REDEX_HASHCODE = DexString::make_string("redex$OE$hashCode");
  const DexString* REDEX_STRING_VALUEOF =
      DexString::make_string("redex$OE$String_valueOf");
  const DexString* REDEX_VALUEOF = DexString::make_string("redex$OE$valueOf");
  const DexString* INIT_METHOD_STR = DexString::make_string("<init>");
  const DexString* VALUES_METHOD_STR = DexString::make_string("values");
  const DexString* VALUEOF_METHOD_STR = DexString::make_string("valueOf");

  const DexType* ENUM_TYPE = type::java_lang_Enum();
  DexType* INT_TYPE = type::_int();
  DexType* INTEGER_TYPE = type::java_lang_Integer();
  DexType* OBJECT_TYPE = type::java_lang_Object();
  DexType* STRING_TYPE = type::java_lang_String();
  DexType* SERIALIZABLE_TYPE = DexType::make_type("Ljava/io/Serializable;");
  DexType* COMPARABLE_TYPE = DexType::make_type("Ljava/lang/Comparable;");
  DexType* RTEXCEPTION_TYPE =
      DexType::make_type("Ljava/lang/RuntimeException;");
  DexType* ILLEGAL_ARG_EXCP_TYPE =
      DexType::make_type("Ljava/lang/IllegalArgumentException;");

  const DexMethodRef* ENUM_ORDINAL_METHOD = method::java_lang_Enum_ordinal();
  const DexMethodRef* ENUM_EQUALS_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.equals:(Ljava/lang/Object;)Z");
  const DexMethodRef* ENUM_COMPARETO_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.compareTo:(Ljava/lang/Enum;)I");
  const DexMethodRef* ENUM_TOSTRING_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.toString:()Ljava/lang/String;");
  const DexMethodRef* ENUM_HASHCODE_METHOD =
      DexMethod::make_method("Ljava/lang/Enum;.hashCode:()I");
  const DexMethodRef* ENUM_NAME_METHOD = method::java_lang_Enum_name();
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
  DexMethodRef* INTEGER_INTVALUE_METHOD = method::java_lang_Integer_intValue();
  DexMethodRef* INTEGER_EQUALS_METHOD = DexMethod::make_method(
      "Ljava/lang/Integer;.equals:(Ljava/lang/Object;)Z");
  DexMethodRef* INTEGER_COMPARETO_METHOD = DexMethod::make_method(
      "Ljava/lang/Integer;.compareTo:(Ljava/lang/Integer;)I");
  DexMethodRef* INTEGER_VALUEOF_METHOD = method::java_lang_Integer_valueOf();
  DexMethodRef* RTEXCEPTION_CTOR_METHOD = DexMethod::make_method(
      "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V");
  DexMethodRef* ILLEGAL_ARG_CONSTRUCT_METHOD = DexMethod::make_method(
      "Ljava/lang/IllegalArgumentException;.<init>:(Ljava/lang/String;)V");
  DexMethodRef* STRING_EQ_METHOD =
      DexMethod::make_method("Ljava/lang/String;.equals:(Ljava/lang/Object;)Z");

  explicit EnumUtil(const Config& config) : m_config(config) {}

  void create_util_class(DexStoresVector* stores, uint32_t fields_count) {
    uint32_t fields_in_primary = std::min(fields_count, m_config.max_enum_size);
    DexClass* cls = make_enumutils_class(fields_in_primary);
    auto& dexen = (*stores)[0].get_dexen()[0];
    dexen.push_back(cls);
  }

  bool is_super_type_of_candidate_enum(DexType* type) {
    return type == ENUM_TYPE || type == OBJECT_TYPE ||
           type == SERIALIZABLE_TYPE || type == COMPARABLE_TYPE;
  }

  /**
   * IF LCandidateEnum; is a candidate enum:
   *  LCandidateEnum; => Ljava/lang/Integer;
   *  [LCandidateEnum; => [Ljava/lang/Integer;
   *  [[LCandidateEnum; => [[Ljava/lang/Integer;
   *  ...
   * IF it is not a candidate enum, return nullptr.
   */
  DexType* try_convert_to_int_type(const EnumAttributeMap& enum_attributes_map,
                                   DexType* type) const {
    uint32_t level = type::get_array_level(type);
    DexType* elem_type = type;
    if (level) {
      elem_type = type::get_array_element_type(type);
    }
    if (enum_attributes_map.count(elem_type)) {
      return level ? type::make_array_type(INTEGER_TYPE, level) : INTEGER_TYPE;
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
    add_substitute_of_tostring(enum_type);
    auto proto = DexProto::make_proto(
        STRING_TYPE, DexTypeList::make_type_list({INTEGER_TYPE}));
    auto method =
        DexMethod::make_method(enum_type, REDEX_STRING_VALUEOF, proto);
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
   * If `Enum.toString` is not overridden, return method ref to
   * LCandidateEnum;.redex$OE$name:(Integer)String, a substitute for
   * LCandidateEnum;.toString:()String. Otherwise return the overriding method.
   * Store the method ref at the same time.
   */
  DexMethodRef* add_substitute_of_tostring(DexType* enum_type) {
    auto method_ref = get_user_defined_tostring_method(type_class(enum_type));
    if (!method_ref) {
      return add_substitute_of_name(enum_type);
    } else {
      auto method = resolve_method(method_ref, MethodSearch::Virtual);
      always_assert(method);
      return method_ref;
    }
  }

  /**
   * If `Enum.toString` is not overridden, return method ref to
   * LCandidateEnum;.redex$OE$name:(Integer)String. Otherwise return the
   * overriding method.
   */
  DexMethodRef* get_substitute_of_tostring(DexType* enum_type) {
    DexMethodRef* method =
        get_user_defined_tostring_method(type_class(enum_type));
    if (!method) {
      return get_substitute_of_name(enum_type);
    }
    return method;
  }

  /**
   * Return method ref to LCandidateEnum;.redex$OE$name:(Integer)String, a
   * substitute for LCandidateEnum;.name:()String.
   * Store the method ref at the same time.
   */
  DexMethodRef* add_substitute_of_name(DexType* enum_type) {
    auto method = get_substitute_of_name(enum_type);
    m_substitute_methods.insert(method);
    return method;
  }

  /**
   * Return method ref to LCandidateEnum;.redex$OE$name:(Integer)String
   */
  DexMethodRef* get_substitute_of_name(DexType* enum_type) {
    auto proto = DexProto::make_proto(
        STRING_TYPE, DexTypeList::make_type_list({INTEGER_TYPE}));
    auto method = DexMethod::make_method(enum_type, REDEX_NAME, proto);
    return method;
  }

  /**
   * Returns a method ref to LCandidateEnum;.redex$OE$hashCode:(Integer)I, a
   * substitute for LCandidateEnum;.hashCode:()I.
   * Store the method ref at the same time.
   */
  DexMethodRef* add_substitute_of_hashcode(DexType* enum_type) {
    // `redex$OE$hashCode()` uses `redex$OE$name()` so we better make sure
    // the method exists.
    add_substitute_of_name(enum_type);
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

  /**
   * Returns a method ref to LCandidateEnum;.redex$OE$get_iField:(Integer)X
   * where `X` is the type of the instance field `iField`.
   * Store the method ref at the same time.
   */
  DexMethodRef* add_get_ifield_method(DexType* enum_type, DexFieldRef* ifield) {
    if (m_get_instance_field_methods.count(ifield)) {
      return m_get_instance_field_methods.at(ifield);
    }
    auto proto = DexProto::make_proto(
        ifield->get_type(), DexTypeList::make_type_list({INTEGER_TYPE}));
    auto method_name = DexString::make_string("redex$OE$get_" + ifield->str());
    auto method = DexMethod::make_method(enum_type, method_name, proto);
    m_get_instance_field_methods.insert(std::make_pair(ifield, method));
    return method;
  }

  /**
   * Returns the `LCandidateEnum.toString()` method that overrides
   * `Enum.toString()`. Return `nullptr` if `Enum.toString()` is not overridden.
   */
  DexMethod* get_user_defined_tostring_method(DexClass* cls) {
    static ConcurrentMap<DexClass*, DexMethod*> cache;
    if (cache.count(cls)) {
      return cache.at(cls);
    }
    for (auto vmethod : cls->get_vmethods()) {
      if (method::signatures_match(vmethod, ENUM_TOSTRING_METHOD)) {
        cache.insert(std::make_pair(cls, vmethod));
        return vmethod;
      }
    }
    cache.insert(std::make_pair(cls, nullptr));
    return nullptr;
  }

 private:
  /**
   * Create a helper class for enums.
   */
  DexClass* make_enumutils_class(uint32_t fields_count) {
    // Note that the EnumUtilsFieldAnalyzer does pattern matching on fields of
    // the form $EnumUtils.fXXX, and should be kept in sync.
    std::string name = "Lredex/$EnumUtils;";
    DexType* type = DexType::get_type(name);
    while (type) {
      name.insert(name.size() - 1, "$u");
      type = DexType::get_type(name);
    }
    type = DexType::make_type(name.c_str());
    ClassCreator cc(type);
    cc.set_access(ACC_PUBLIC | ACC_FINAL);
    cc.set_super(type::java_lang_Object());
    DexClass* cls = cc.create();
    cls->rstate.set_generated();
    cls->rstate.set_clinit_has_no_side_effects();

    auto values_field = make_values_field(cls);
    auto clinit_method = make_clinit_method(cls, fields_count);
    auto clinit_code = clinit_method->get_code();
    m_fields.reserve(fields_count);
    for (uint32_t i = 0; i < fields_count; ++i) {
      m_fields.push_back(make_a_field(cls, i, clinit_code));
    }

    clinit_code->push_back(dasm(OPCODE_SPUT_OBJECT, values_field, {2_v}));
    clinit_code->push_back(dasm(OPCODE_RETURN_VOID));

    m_values_method_ref = make_values_method(cls, values_field, fields_count);

    return cls;
  }

  /**
   * LEnumUtils;.$VALUES:[Ljava/lang/Integer;
   */
  DexFieldRef* make_values_field(DexClass* cls) {
    auto name = DexString::make_string("$VALUES");
    auto field = DexField::make_field(cls->get_type(), name,
                                      type::make_array_type(INTEGER_TYPE))
                     ->make_concrete(ACC_PRIVATE | ACC_FINAL | ACC_STATIC);
    cls->add_field(field);
    field->set_deobfuscated_name(show_deobfuscated(field));
    return (DexFieldRef*)field;
  }

  /**
   * Create a static final Integer field and update <clinit> code.
   */
  DexFieldRef* make_a_field(DexClass* cls, uint32_t value, IRCode* code) {
    // Note that the EnumUtilsFieldAnalyzer does pattern matching on fields of
    // the form $EnumUtils.fXXX, and should be kept in sync.
    auto name = DexString::make_string("f" + std::to_string(value));
    auto field = DexField::make_field(cls->get_type(), name, INTEGER_TYPE)
                     ->make_concrete(ACC_PUBLIC | ACC_FINAL | ACC_STATIC);
    cls->add_field(field);
    field->set_deobfuscated_name(show_deobfuscated(field));
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
        DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
    DexMethod* method =
        DexMethod::make_method(cls->get_type(), CLINIT_METHOD_STR, proto)
            ->make_concrete(ACC_STATIC | ACC_CONSTRUCTOR, false);
    method->set_code(std::make_unique<IRCode>());
    cls->add_method(method);
    method->set_deobfuscated_name(show_deobfuscated(method));
    auto code = method->get_code();

    // const v2, xx
    // new-array v2, v2, [Integer
    code->push_back(dasm(OPCODE_CONST, {2_v, {LITERAL, fields_count}}));
    code->push_back(
        dasm(OPCODE_NEW_ARRAY, type::make_array_type(INTEGER_TYPE), {2_v}));
    code->push_back(dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {2_v}));
    code->set_registers_size(3);
    return method;
  }

  /**
   * LEnumUtils;.values:(I)[Ljava/lang/Integer;
   *
   * We construct an array field at class loading time, which stores some of the
   * integers. Copy part of the array if the required integers are in the array,
   * otherwise copy all of them and construct more. The following comments are
   * the basic blocks of this method.
   *
   * res = new Integer[count]
   * if count <= VALUES.length
   *   : small_argument_block
   *   copy_size = count
   *   goto :copy_array_block
   * else
   *   : large_argument_block
   *   copy_size = VALUES.length
   *   id = copy_size
   *   goto :integers_block
   *   : integers_block
   *   if id < count
   *     : one_integer_block
   *     res[id] = Integer.valueOf(id)
   *     id = id + 1
   *     goto :integers_block
   *   else
   *     goto :copy_array_block
   * : copy_array_block
   * System.arraycopy(VALUES, 0, res, 0, copy_size);
   * return res
   */
  DexMethodRef* make_values_method(DexClass* cls,
                                   DexFieldRef* values_field,
                                   uint32_t total_integer_fields) {
    auto name = DexString::make_string("values");
    auto integer_array_type = type::make_array_type(INTEGER_TYPE);
    DexProto* proto = DexProto::make_proto(
        integer_array_type, DexTypeList::make_type_list({type::_int()}));
    DexMethod* method = DexMethod::make_method(cls->get_type(), name, proto)
                            ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    method->set_code(std::make_unique<IRCode>(method, 0));
    cls->add_method(method);
    method->set_deobfuscated_name(show_deobfuscated(method));
    auto code = method->get_code();
    code->build_cfg();
    auto& cfg = code->cfg();
    auto entry = cfg.entry_block();
    auto small_argument_block = cfg.create_block();
    auto large_argument_block = cfg.create_block();
    auto one_integer_block = cfg.create_block();
    auto integers_block = cfg.create_block();
    auto copy_array_block = cfg.create_block();
    cfg.add_edge(small_argument_block, copy_array_block, cfg::EDGE_GOTO);
    cfg.add_edge(large_argument_block, integers_block, cfg::EDGE_GOTO);
    cfg.add_edge(one_integer_block, integers_block, cfg::EDGE_GOTO);

    entry->push_back(
        {dasm(OPCODE_NEW_ARRAY, integer_array_type, {0_v}),
         dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {1_v}),
         dasm(OPCODE_CONST, {2_v, {LITERAL, total_integer_fields}})});
    cfg.create_branch(entry, dasm(OPCODE_IF_LE, {0_v, 2_v}),
                      large_argument_block, small_argument_block);

    small_argument_block->push_back(dasm(OPCODE_MOVE, {4_v, 0_v}));

    large_argument_block->push_back(
        {dasm(OPCODE_MOVE, {4_v, 2_v}), dasm(OPCODE_MOVE, {5_v, 2_v})});
    cfg.create_branch(integers_block, dasm(OPCODE_IF_LT, {5_v, 0_v}),
                      copy_array_block, one_integer_block);

    one_integer_block->push_back(
        {dasm(OPCODE_INVOKE_STATIC, INTEGER_VALUEOF_METHOD, {5_v}),
         dasm(OPCODE_MOVE_RESULT_OBJECT, {6_v}),
         dasm(OPCODE_APUT_OBJECT, {6_v, 1_v, 5_v}),
         dasm(OPCODE_ADD_INT_LIT8, {5_v, 5_v, 1_L})});

    auto copy_array_method = DexMethod::make_method(
        "Ljava/lang/System;.arraycopy:(Ljava/lang/Object;ILjava/lang/"
        "Object;II)V");
    copy_array_block->push_back({dasm(OPCODE_SGET_OBJECT, values_field),
                                 dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {7_v}),
                                 dasm(OPCODE_CONST, {8_v, 0_L}),
                                 dasm(OPCODE_INVOKE_STATIC, copy_array_method,
                                      {7_v, 8_v, 1_v, 8_v, 4_v}),
                                 dasm(OPCODE_RETURN_OBJECT, {1_v})});
    cfg.recompute_registers_size();
    code->clear_cfg();
    return (DexMethodRef*)method;
  }
};

struct InsnReplacement {
  cfg::InstructionIterator original_insn;
  std::vector<IRInstruction*> replacements;

  InsnReplacement(cfg::ControlFlowGraph& cfg,
                  cfg::Block* block,
                  MethodItemEntry* mie,
                  IRInstruction* new_insn)
      : original_insn(block->to_cfg_instruction_iterator(*mie)),
        replacements{new_insn} {
    push_back_move_result(cfg, new_insn);
  }
  InsnReplacement(cfg::ControlFlowGraph& cfg,
                  cfg::Block* block,
                  MethodItemEntry* mie,
                  std::vector<IRInstruction*>& new_insns)
      : original_insn(block->to_cfg_instruction_iterator(*mie)),
        replacements(std::move(new_insns)) {
    if (!replacements.empty()) {
      auto new_insn = *replacements.rbegin();
      push_back_move_result(cfg, new_insn);
    }
  }

 private:
  /**
   * If the original instruction was paired with a `move-result`, create a new
   * one with the same destination register (and possibly the same opcode)
   * because the original one will be removed.
   */
  void push_back_move_result(cfg::ControlFlowGraph& cfg,
                             IRInstruction* new_insn) {
    auto org_move_insn_it = cfg.move_result_of(original_insn);
    if (!org_move_insn_it.is_end()) {
      auto& org_move_insn = org_move_insn_it.unwrap()->insn;
      auto& org_insn = original_insn.unwrap()->insn;
      auto org_op = org_move_insn->opcode();

      auto dest = org_move_insn->dest();
      IROpcode new_op = org_op;
      if (org_insn->has_move_result() && new_insn->has_move_result_pseudo()) {
        new_op = opcode::move_result_to_pseudo(org_op);
      } else if (org_insn->has_move_result_pseudo() &&
                 new_insn->has_move_result()) {
        new_op = opcode::pseudo_to_move_result(org_op);
      }
      replacements.push_back(dasm(new_op, {{VREG, dest}}));
    }
  }
};

/**
 * Code transformation for a method.
 */
class CodeTransformer final {
 public:
  CodeTransformer(const EnumAttributeMap& m_enum_attributes_map,
                  EnumUtil* enum_util,
                  DexMethod* method)
      : m_enum_attributes_map(m_enum_attributes_map),
        m_enum_util(enum_util),
        m_method(method) {}

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
    case OPCODE_IGET:
    case OPCODE_IGET_WIDE:
    case OPCODE_IGET_OBJECT:
    case OPCODE_IGET_BOOLEAN:
    case OPCODE_IGET_BYTE:
    case OPCODE_IGET_CHAR:
    case OPCODE_IGET_SHORT:
      update_iget(cfg, block, mie);
      break;
    case OPCODE_INVOKE_VIRTUAL: {
      auto method = insn->get_method();
      if (method::signatures_match(method, m_enum_util->ENUM_ORDINAL_METHOD)) {
        update_invoke_virtual(env, cfg, block, mie,
                              m_enum_util->INTEGER_INTVALUE_METHOD);
      } else if (method::signatures_match(method,
                                          m_enum_util->ENUM_EQUALS_METHOD)) {
        update_invoke_virtual(env, cfg, block, mie,
                              m_enum_util->INTEGER_EQUALS_METHOD);
      } else if (method::signatures_match(method,
                                          m_enum_util->ENUM_COMPARETO_METHOD)) {
        update_invoke_virtual(env, cfg, block, mie,
                              m_enum_util->INTEGER_COMPARETO_METHOD);
      } else if (method::signatures_match(method,
                                          m_enum_util->ENUM_NAME_METHOD)) {
        update_invoke_name(env, cfg, block, mie);
      } else if (method::signatures_match(method,
                                          m_enum_util->ENUM_HASHCODE_METHOD)) {
        update_invoke_hashcode(env, cfg, block, mie);
      } else if (method == m_enum_util->STRINGBUILDER_APPEND_OBJ_METHOD) {
        update_invoke_stringbuilder_append(env, cfg, block, mie);
      } else {
        update_invoke_user_method(env, cfg, block, mie);
      }
    } break;
    case OPCODE_INVOKE_DIRECT: {
      auto method = insn->get_method();
      if (!method::is_init(method)) {
        update_invoke_user_method(env, cfg, block, mie);
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
      auto new_type = try_convert_to_int_type(type);
      if (new_type) {
        auto possible_src_types = env.get(insn->src(0));
        if (possible_src_types.size() != 0) {
          DexType* candidate_type =
              extract_candidate_enum_type(possible_src_types);
          always_assert(candidate_type == type);
        }
        // Empty src_types means the src register holds null object.
        insn->set_type(new_type);
      } else if (type == m_enum_util->ENUM_TYPE) {
        always_assert(!extract_candidate_enum_type(env.get(insn->src(0))));
      }
    } break;
    default: {
      if (insn->has_type() && insn->opcode() != IOPCODE_INIT_CLASS) {
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
   * or
   *   const v_ordinal #??
   *   invoke-static v_ordinal Integer.valueOf:(I)Integer
   */
  void update_sget_object(const optimize_enums::EnumTypeEnvironment& env,
                          cfg::ControlFlowGraph& cfg,
                          cfg::Block* block,
                          MethodItemEntry* mie) {
    auto insn = mie->insn;
    auto field = insn->get_field();
    if (!m_enum_attributes_map.count(field->get_type())) {
      return;
    }
    auto& constants =
        m_enum_attributes_map.at(field->get_type()).m_constants_map;
    if (!constants.count(field)) {
      return;
    }
    uint32_t ordinal = constants.at(field).ordinal;
    if (ordinal < m_enum_util->m_config.max_enum_size) {
      auto new_field = m_enum_util->m_fields.at(constants.at(field).ordinal);
      auto new_insn = dasm(OPCODE_SGET_OBJECT, new_field);
      m_replacements.push_back(InsnReplacement(cfg, block, mie, new_insn));
    } else {
      always_assert(
          m_enum_util->m_config.breaking_reference_equality_allowlist.count(
              field->get_type()));
      auto ordinal_reg = allocate_temp();
      std::vector<IRInstruction*> new_insns;
      new_insns.push_back(
          dasm(OPCODE_CONST, {{VREG, ordinal_reg}, {LITERAL, ordinal}}));
      new_insns.push_back(dasm(OPCODE_INVOKE_STATIC,
                               m_enum_util->INTEGER_VALUEOF_METHOD,
                               {{VREG, ordinal_reg}}));
      m_replacements.push_back(InsnReplacement(cfg, block, mie, new_insns));
    }
  }

  /**
   * If the instance field belongs to a CandidateEnum, replace the `iget`
   * instruction with a static call to the correct method.
   *
   * iget(-object|-wide)? vObj LCandidateEnum;.iField:Ltype;
   * move-result-pseudo vDest
   * =>
   * invoke-static {vObj}, LCandidateEnum;.redex$OE$get_iField:(Integer;)Ltype;
   * move-result(-object|-wide)? vDest
   */
  void update_iget(cfg::ControlFlowGraph& cfg,
                   cfg::Block* block,
                   MethodItemEntry* mie) {
    auto insn = mie->insn;
    auto ifield = insn->get_field();
    auto enum_type = ifield->get_class();
    if (!m_enum_attributes_map.count(enum_type)) {
      return;
    }
    auto vObj = insn->src(0);
    auto get_ifield_method =
        m_enum_util->add_get_ifield_method(enum_type, ifield);
    m_replacements.push_back(InsnReplacement(
        cfg, block, mie,
        dasm(OPCODE_INVOKE_STATIC, get_ifield_method, {{VREG, vObj}})));
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
    auto attributes_it = m_enum_attributes_map.find(container);
    if (attributes_it != m_enum_attributes_map.end()) {
      auto reg = allocate_temp();
      uint64_t enum_size = attributes_it->second.m_constants_map.size();
      always_assert(enum_size);
      std::vector<IRInstruction*> new_insns;
      new_insns.push_back(
          dasm(OPCODE_CONST,
               {{VREG, reg}, {LITERAL, static_cast<int64_t>(enum_size)}}));
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
    if (!m_enum_attributes_map.count(container)) {
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
   * invoke-virtual v0 LCandidateEnum;.name:()Ljava/lang/String; or
   * invoke-virtual v0 LCandidateEnum;.toString:()Ljava/lang/String; =>
   *    invoke-static v0 LCandidateEnum;.redex$OE$name:(Integer)String
   */
  void update_invoke_name(const optimize_enums::EnumTypeEnvironment& env,
                          cfg::ControlFlowGraph& cfg,
                          cfg::Block* block,
                          MethodItemEntry* mie) {
    auto insn = mie->insn;
    auto container = insn->get_method()->get_class();
    auto reg = insn->src(0);
    auto candidate_type = infer_candidate_type(env.get(reg), container);
    if (!candidate_type) {
      return;
    }
    auto helper_method = m_enum_util->add_substitute_of_name(candidate_type);
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
    auto src_reg = insn->src(0);
    auto candidate_type = infer_candidate_type(env.get(src_reg), container);
    if (!candidate_type) {
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
    auto src_reg = insn->src(0);
    auto candidate_type = infer_candidate_type(env.get(src_reg), container);
    if (!candidate_type) {
      return;
    }
    auto new_insn = new IRInstruction(OPCODE_INVOKE_VIRTUAL);
    new_insn->set_method(integer_meth)->set_srcs_size(insn->srcs_size());
    for (size_t id = 0; id < insn->srcs_size(); ++id) {
      new_insn->set_src(id, insn->src(id));
    }
    m_replacements.push_back(InsnReplacement(cfg, block, mie, new_insn));
  }

  /**
   * If this is an invocation of a user-defined virtual or direct method on a
   * CandidateEnum, then we make that method static. If that method is
   * toString(), then we call one of the appropriate methods Enum.name()
   * or CandidateEnum.toString(). Otherwise we do nothing.
   */
  void update_invoke_user_method(const optimize_enums::EnumTypeEnvironment& env,
                                 cfg::ControlFlowGraph& cfg,
                                 cfg::Block* block,
                                 MethodItemEntry* mie) {
    auto insn = mie->insn;
    auto method_ref = insn->get_method();
    auto container_type = method_ref->get_class();
    auto candidate_type =
        infer_candidate_type(env.get(insn->src(0)), container_type);
    if (!candidate_type) {
      return;
    }

    // If this is toString() and there is no CandidateEnum.toString(), then we
    // call Enum.name() instead.
    if (method::signatures_match(method_ref,
                                 m_enum_util->ENUM_TOSTRING_METHOD) &&
        m_enum_util->get_user_defined_tostring_method(
            type_class(candidate_type)) == nullptr) {
      update_invoke_name(env, cfg, block, mie);
    } else {
      auto method = resolve_method(method_ref, opcode_to_search(insn));
      always_assert(method);
      auto new_insn = (new IRInstruction(*insn))
                          ->set_opcode(OPCODE_INVOKE_STATIC)
                          ->set_method(method);
      m_replacements.push_back(InsnReplacement(cfg, block, mie, new_insn));
    }
  }

  /**
   * Infer a candidate type from an instruction like
   * `invoke-virtual vReg, Target.method()`
   *
   * Return a candidate type if we can get only one, return null if all these
   * types are not related to our candidate types. Bail out if the type are
   * mixed (our analysis part should have excluded this case).
   */
  DexType* infer_candidate_type(const EnumTypes& reg_types,
                                DexType* target_type) {
    DexType* candidate_type = nullptr;
    if (is_a_candidate(target_type)) {
      candidate_type = target_type;
    } else if (!m_enum_util->is_super_type_of_candidate_enum(target_type)) {
      return nullptr;
    }
    auto type_set = reg_types.elements();
    if (type_set.empty()) {
      // Register holds null value, we infer the type in instruction.
      return candidate_type;
    } else if (candidate_type) {
      always_assert_log(type_set.size() == 1 &&
                            *type_set.begin() == candidate_type,
                        "%s != %s", SHOW(type_set), SHOW(candidate_type));
      return candidate_type;
    } else if (type_set.size() == 1) {
      candidate_type = *type_set.begin();
      return is_a_candidate(candidate_type) ? candidate_type : nullptr;
    } else {
      for (auto t : type_set) {
        always_assert_log(!is_a_candidate(t), "%s\n", SHOW(t));
      }
      return nullptr;
    }
  }

  /**
   * Return nullptr if the types contain none of the candidate enums,
   * return the candidate type if types only contain one candidate enum and do
   * not contain other types,
   * or assertion failure when the types are mixed.
   */
  DexType* extract_candidate_enum_type(const EnumTypes& types) {
    return infer_candidate_type(types, m_enum_util->OBJECT_TYPE);
  }

  DexType* try_convert_to_int_type(DexType* type) {
    return m_enum_util->try_convert_to_int_type(m_enum_attributes_map, type);
  }

  bool is_a_candidate(DexType* type) const {
    auto elem_type =
        const_cast<DexType*>(type::get_element_type_if_array(type));
    return m_enum_attributes_map.count(elem_type);
  }

  inline reg_t allocate_temp() {
    return m_method->get_code()->cfg().allocate_temp();
  }

  const EnumAttributeMap& m_enum_attributes_map;
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
  EnumTransformer(const Config& config, DexStoresVector* stores)
      : m_stores(*stores), m_int_objs(0) {
    m_enum_util = std::make_unique<EnumUtil>(config);
    for (auto it = config.candidate_enums.begin();
         it != config.candidate_enums.end();
         ++it) {
      auto enum_cls = type_class(*it);
      auto attributes = optimize_enums::analyze_enum_clinit(enum_cls);
      size_t num_enum_constants = attributes.m_constants_map.size();
      if (num_enum_constants == 0) {
        TRACE(ENUM, 2, "\tCannot analyze enum %s : ord %lu sfields %lu",
              SHOW(enum_cls), num_enum_constants,
              enum_cls->get_sfields().size());
        continue;
      } else if (num_enum_constants > config.max_enum_size) {
        if (!config.breaking_reference_equality_allowlist.count(*it)) {
          TRACE(ENUM, 2, "\tSkip %s %lu values", SHOW(enum_cls),
                num_enum_constants);
          continue;
        } else {
          TRACE(ENUM, 2,
                "\tOptimimze %s (%lu values) but object equality is not "
                "guaranteed",
                SHOW(enum_cls), num_enum_constants);
        }
      }
      m_int_objs = std::max<uint32_t>(m_int_objs, num_enum_constants);
      m_enum_objs += num_enum_constants;
      m_enum_attributes_map.emplace(*it, attributes);
      clean_generated_methods_fields(enum_cls);
      opt_metadata::log_opt(ENUM_OPTIMIZED, enum_cls);
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
          if (m_enum_attributes_map.count(method->get_class()) &&
              is_generated_enum_method(method)) {
            return false;
          }
          std::vector<DexType*> types;
          method->gather_types(types);
          return std::any_of(types.begin(), types.end(), [this](DexType* type) {
            return (bool)try_convert_to_int_type(type);
          });
        },
        [&](DexMethod* method, IRCode& code) {
          if (m_enum_attributes_map.count(method->get_class()) &&
              (!is_constructor(method) && !is_static(method))) {
            m_enum_util->m_instance_methods.insert(method);
          }
          CodeTransformer code_updater(m_enum_attributes_map, m_enum_util.get(),
                                       method);
          code_updater.run();
        });
    create_substitute_methods(m_enum_util->m_substitute_methods);
    std::vector<DexMethod*> instance_methods(
        m_enum_util->m_instance_methods.begin(),
        m_enum_util->m_instance_methods.end());
    std::sort(instance_methods.begin(), instance_methods.end(),
              dexmethods_comparator());
    for (auto method : instance_methods) {
      mutators::make_static(method);
    }
    std::map<DexFieldRef*, DexMethodRef*, dexfields_comparator> field_to_method(
        m_enum_util->m_get_instance_field_methods.begin(),
        m_enum_util->m_get_instance_field_methods.end());
    for (auto& pair : field_to_method) {
      create_get_instance_field_method(pair.second, pair.first);
    }
    post_update_enum_classes(scope);
    // Update all methods and fields references by replacing the candidate enum
    // types with Integer type.
    std::unordered_map<DexType*, DexType*> type_mapping;
    for (auto& pair : m_enum_attributes_map) {
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
          if (m_enum_attributes_map.count(container)) {
            always_assert_log(method_ref->is_def(), "Invalid insn %s in %s\n",
                              SHOW(insn), SHOW(method));
          }
        } else if (insn->has_field()) {
          auto field_ref = insn->get_field();
          auto container = field_ref->get_class();
          if (m_enum_attributes_map.count(container)) {
            always_assert_log(field_ref->is_def(), "Invalid insn %s in %s\n",
                              SHOW(insn), SHOW(method));
          }
        } else if (insn->has_type() && insn->opcode() != IOPCODE_INIT_CLASS) {
          auto type_ref = insn->get_type();
          always_assert_log(!try_convert_to_int_type(type_ref),
                            "Invalid insn %s in %s\n", SHOW(insn),
                            SHOW(method));
        }
      }
    });
  }

  void create_substitute_methods(const ConcurrentSet<DexMethodRef*>& methods) {
    for (auto ref : methods) {
      if (ref->get_name() == m_enum_util->REDEX_NAME) {
        create_name_method(ref);
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
   *   return CandidateEnum.toString(obj);
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
    for (auto& pair :
         m_enum_attributes_map[ref->get_class()].get_ordered_names()) {
      prev_block->push_back({dasm(OPCODE_CONST_STRING, pair.second),
                             dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {1_v}),
                             dasm(OPCODE_INVOKE_VIRTUAL,
                                  m_enum_util->STRING_EQ_METHOD, {0_v, 1_v}),
                             dasm(OPCODE_MOVE_RESULT, {3_v})});

      auto equal_block = cfg.create_block();
      {
        auto obj_field = m_enum_util->m_fields[pair.first];
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
   * Substitute for LCandidateEnum;.name()
   *
   * public static String redex$OE$name(Integer obj) {
   *   switch(obj.intValue()) {
   *     case 0 : ...;
   *     case 1 : ...;
   *     ...
   *   }
   * }
   */
  void create_name_method(DexMethodRef* ref) {
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

    std::vector<std::pair<int32_t, cfg::Block*>> cases;
    for (auto& pair :
         m_enum_attributes_map[ref->get_class()].get_ordered_names()) {
      auto block = cfg.create_block();
      cases.emplace_back(pair.first, block);
      block->push_back({dasm(OPCODE_CONST_STRING, pair.second),
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
   *   String name = CandidateEnum.name(obj);
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
    auto name_method = m_enum_util->get_substitute_of_name(ref->get_class());
    entry->push_back({
        dasm(OPCODE_INVOKE_STATIC, name_method, {0_v}),
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
   * Create a helper method to replace `iget` instructions that returns an
   * instance field value given the enum ordinal.
   *
   * public static [type] redex$OE$get_instanceField(Integer obj) {
   *   switch (obj.intValue()) {
   *     case 0: return value0;
   *     case 1: return value1;
   *     ...
   *   }
   * }
   */
  void create_get_instance_field_method(DexMethodRef* method_ref,
                                        DexFieldRef* ifield_ref) {
    MethodCreator mc(method_ref, ACC_STATIC | ACC_PUBLIC);
    auto method = mc.create();
    auto cls = type_class(method_ref->get_class());
    cls->add_method(method);
    auto code = method->get_code();
    code->build_cfg();
    auto& cfg = code->cfg();
    auto entry = cfg.entry_block();
    entry->push_back({dasm(OPCODE_INVOKE_VIRTUAL,
                           m_enum_util->INTEGER_INTVALUE_METHOD, {0_v}),
                      dasm(OPCODE_MOVE_RESULT, {0_v})});
    auto ifield_type = ifield_ref->get_type();
    std::vector<std::pair<int32_t, cfg::Block*>> cases;
    for (auto& pair :
         m_enum_attributes_map[cls->get_type()].m_field_map[ifield_ref]) {
      auto ordinal = pair.first;
      auto block = cfg.create_block();
      cases.emplace_back(ordinal, block);
      if (ifield_type == type::java_lang_String()) {
        const DexString* value = pair.second.string_value;
        if (value) {
          block->push_back({dasm(OPCODE_CONST_STRING, value),
                            dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {1_v}),
                            dasm(OPCODE_RETURN_OBJECT, {1_v})});
        } else {
          // The `Ljava/lang/String` value is a `null` constant.
          block->push_back({dasm(OPCODE_CONST, {1_v, 0_L}),
                            dasm(OPCODE_RETURN_OBJECT, {1_v})});
        }
      } else {
        int64_t value = pair.second.primitive_value;
        if (type::is_wide_type(ifield_type)) {
          block->push_back({dasm(OPCODE_CONST_WIDE, {1_v, {LITERAL, value}}),
                            dasm(OPCODE_RETURN_WIDE, {1_v})});
        } else {
          block->push_back({dasm(OPCODE_CONST, {1_v, {LITERAL, value}}),
                            dasm(OPCODE_RETURN, {1_v})});
        }
      }
    }
    // Arbitrarily choose the first case block as the default case.
    always_assert(!cases.empty());
    cfg.create_branch(entry, dasm(OPCODE_SWITCH, {0_v}), cases.front().second,
                      cases);
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
    auto& enum_constants =
        m_enum_attributes_map[enum_cls->get_type()].m_constants_map;
    auto synth_field_access = synth_access();
    DexField* values_field = nullptr;

    std20::erase_if(sfields, [&](auto* field) {
      if (enum_constants.count(field)) {
        return true;
      }
      if (check_required_access_flags(synth_field_access,
                                      field->get_access())) {
        always_assert(!values_field);
        values_field = field;
        return true;
      }
      return false;
    });

    always_assert(values_field);
    auto& dmethods = enum_cls->get_dmethods();
    // Delete <init>, values() and valueOf(String) methods, and clean <clinit>.
    std20::erase_if(dmethods, [&, this](auto* method) {
      if (method::is_clinit(method)) {
        clean_clinit(enum_constants, enum_cls, method, values_field);
        return empty(method->get_code());
      }
      return this->is_generated_enum_method(method);
    });
  }

  /**
   * Erase enum construction code. Erase the put instructions that write enum
   * values and synthetic $VALUES array, then erase the dead instructions.
   *
   * The code before the transformation:
   *
   * new-instance v0 LCandidateEnum;
   * invoke-direct v0 v1 v2 Ljava/lang/Enum;.<init>:(Ljava/lang/String;I)V
   * sput-object v0 LCandidateEnum;.f:LCandidateEnum;
   * ... // maybe more objects construction.
   * sput-object v3 LCandidateEnum;.$VALUES:[LCandidateEnum;
   * ... // register v0 may be used.
   *
   * The code after the transformation:
   *
   * // Deleted. new-instance v0 LCandidateEnum;
   * // Deleted. invoke-direct v0 v1 v2
   * Ljava/lang/Enum;.<init>:(Ljava/lang/String;I)V
   * // Deleted. sput-object v0 LCandidateEnum;.f:LCandidateEnum;
   * sget-object v0 LCandidateEnum;.f:LCandidateEnum;
   * ... // maybe more objects construction.
   * // Deleted. sput-object v3 LCandidateEnum;.$VALUES:[LCandidateEnum;
   * ... // register v0 may be used.
   */
  static void clean_clinit(const EnumConstantsMap& enum_constants,
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
      if (opcode::is_an_sput(insn->opcode())) {
        auto field = resolve_field(insn->get_field());
        if (field && enum_constants.count(field)) {
          code->insert_before(it, dasm(OPCODE_SGET_OBJECT, field));
          code->insert_before(
              it,
              dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {{VREG, insn->src(0)}}));
          it = code->erase(it);
        } else if (field == values_field) {
          it = code->erase(it);
        }
      } else if (opcode::is_invoke_direct(insn->opcode()) &&
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
    for (const auto& insn : dead_instructions) {
      code->remove_opcode(insn);
    }
    // Assert no instruction about the $VALUES field.
    for (auto& mie : InstructionIterable(code)) {
      auto insn = mie.insn;
      always_assert_log(!insn->has_field() || insn->get_field() != values_field,
                        "%s can not be deleted", SHOW(insn));
    }
  }

  /**
   * Only use for <clinit> code.
   */
  static bool empty(IRCode* code) {
    auto iterable = InstructionIterable(code);
    auto begin = iterable.begin();
    return opcode::is_return_void(begin->insn->opcode());
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
      if (!m_enum_attributes_map.count(cls->get_type())) {
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
    return m_enum_util->try_convert_to_int_type(m_enum_attributes_map, type);
  }

  DexStoresVector& m_stores;
  uint32_t m_int_objs{0}; // Generated Integer objects.
  uint32_t m_enum_objs{0}; // Eliminated Enum objects.
  EnumAttributeMap m_enum_attributes_map;
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
                    size_t* num_int_objs) {
  if (!config.candidate_enums.size()) {
    return 0;
  }

  EnumTransformer transformer(config, stores);
  transformer.run();
  *num_int_objs = transformer.get_int_objs_count();
  return transformer.get_enum_objs_count();
}
} // namespace optimize_enums
