/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OptimizeEnums.h"

#include "ClassAssemblingUtils.h"
#include "ConcurrentContainers.h"
#include "DexClass.h"
#include "EnumInSwitch.h"
#include "EnumTransformer.h"
#include "EnumUpcastAnalysis.h"
#include "IRCode.h"
#include "OptimizeEnumsAnalysis.h"
#include "OptimizeEnumsGeneratedAnalysis.h"
#include "Resolver.h"
#include "SwitchEquivFinder.h"
#include "Walkers.h"

/**
 * 1. The pass tries to remove synthetic switch map classes for enums
 * completely, by replacing the access to thelookup table with the use of the
 * enum ordinal itself.
 * Background of synthetic switch map classes:
 *   javac converts enum switches to a packed switch. In order to do this, for
 *   every use of an enum in a switch statement, an anonymous class is generated
 *   in the class the switchis defined. This class will contain ONLY lookup
 *   tables (array) as static fields and a static initializer.
 *
 * 2. Try to replace enum objects with boxed Integer objects based on static
 * analysis results.
 */

namespace {

using GeneratedSwitchCases =
    std::unordered_map<DexField*, std::unordered_map<size_t, DexField*>>;

constexpr const char* METRIC_NUM_SYNTHETIC_CLASSES = "num_synthetic_classes";
constexpr const char* METRIC_NUM_LOOKUP_TABLES = "num_lookup_tables";
constexpr const char* METRIC_NUM_LOOKUP_TABLES_REMOVED =
    "num_lookup_tables_replaced";
constexpr const char* METRIC_NUM_ENUM_CLASSES = "num_candidate_enum_classes";
constexpr const char* METRIC_NUM_ENUM_OBJS = "num_enum_objs";
constexpr const char* METRIC_NUM_INT_OBJS = "num_generated_int_objs";

/**
 * Get the instruction containing the constructor call. It can either
 * be the constructor of the superclass or from the same class.
 */
IRInstruction* get_ctor_call(const DexMethod* method,
                             const DexMethod* java_enum_ctor) {
  auto* code = method->get_code();
  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (!is_invoke_direct(insn->opcode())) {
      continue;
    }

    auto method_inv =
        resolve_method(insn->get_method(), opcode_to_search(insn));
    if (method_inv == java_enum_ctor) {
      return insn;
    }

    if (is_init(method_inv) && method_inv->get_class() == method->get_class()) {
      return insn;
    }
  }

  return nullptr;
}

/**
 * Creates a map from register used to the associated argument.
 *
 * For example for :
 *  static void foo(int a, String b) {
 *    OPCODE_LOAD_PARAM <v_a>
 *    OPCODE_LOAD_PARAM_OBJECT <v_b>
 *    ...
 *  }
 *
 *  will return: {
 *    <v_a> -> 0
 *    <v_b> -> 1
 *  }
 */
std::unordered_map<size_t, uint32_t> collect_reg_to_arg(
    const DexMethod* method) {
  auto* code = method->get_code();
  auto params = code->get_param_instructions();
  std::unordered_map<size_t, uint32_t> reg_to_arg;

  size_t arg_index = 0;
  for (const auto& mie : InstructionIterable(params)) {
    auto load_insn = mie.insn;
    always_assert(opcode::is_load_param(load_insn->opcode()));

    reg_to_arg[load_insn->dest()] = arg_index++;
  }

  return reg_to_arg;
}

/**
 * Returns false if the given register is overwritten (aka is used
 * as the destination), except for the load param opcodes.
 */
bool check_ordinal_usage(const DexMethod* method, size_t reg) {
  always_assert(method && method->get_code());

  auto code = method->get_code();
  for (const auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (opcode::is_load_param(insn->opcode())) {
      // Skip load params. We already analyzed those.
      continue;
    }

    if (insn->dests_size() > 0 && insn->dest() == reg) {
      return false;
    }
  }

  return true;
}

/**
 * Simple analysis to determine which of the enums ctor argument
 * is passed for the ordinal.
 *
 * Background: The ordinal for each enum instance is set through the
 *             super class's constructor.
 *
 * Here we determine for each constructor, which of the arguments is used
 * to set the ordinal.
 */
bool analyze_enum_ctors(
    const DexClass* cls,
    const DexMethod* java_enum_ctor,
    std::unordered_map<const DexMethod*, uint32_t>& ctor_to_arg_ordinal) {

  // For each ctor, get the initialization instruction (it might be a call
  // to `Enum.<init>(String;I)` or to a different ctor of the same class.
  std::deque<std::pair<DexMethod*, IRInstruction*>> ctor_to_enum_insn;
  for (const auto& ctor : cls->get_ctors()) {
    auto code = ctor->get_code();
    if (!code) {
      return false;
    }

    auto enum_insn = get_ctor_call(ctor, java_enum_ctor);
    if (!enum_insn) {
      return false;
    }

    ctor_to_enum_insn.emplace_back(ctor, enum_insn);
  }

  // Ordinal represents the third argument.
  // details: https://developer.android.com/reference/java/lang/Enum.html
  ctor_to_arg_ordinal[java_enum_ctor] = 2;

  // TODO: We could order them instead of looping ...
  while (!ctor_to_enum_insn.empty()) {
    auto& pair = ctor_to_enum_insn.front();
    ctor_to_enum_insn.pop_front();

    auto ctor = pair.first;
    auto enum_insn = pair.second;

    auto ctor_called =
        resolve_method(enum_insn->get_method(), MethodSearch::Direct);
    if (!ctor_to_arg_ordinal.count(ctor_called)) {
      ctor_to_enum_insn.push_back(pair);
      continue;
    }

    auto ordinal_reg = enum_insn->src(ctor_to_arg_ordinal[ctor_called]);

    // determine arg -> reg from IOPCODE_LOAD_* opcodes.
    auto reg_to_arg = collect_reg_to_arg(ctor);
    if (reg_to_arg.count(ordinal_reg) == 0) {
      // TODO: A proper analysis wouldn't fail here.
      return false;
    }

    // Check that the register used to store the ordinal
    // is not overwritten.
    if (!check_ordinal_usage(ctor, ordinal_reg)) {
      return false;
    }

    ctor_to_arg_ordinal[ctor] = reg_to_arg[ordinal_reg];
  }

  return true;
}

/**
 * Collect enum fields to switch case.
 *
 * Background: `lookup_table` is bound to a list of integers, that maps from
 * enum field ordinal to a switch case. `<clinit>` initializes this field as:
 * `aput <v_case>, <v_field>, <v_ordinal>` where <v_case> holds the switch case,
 * <v_field> a reference to `field` and <v_ordinal> holds the ordinal of an enum
 * field.
 */
void collect_generated_switch_cases(
    GeneratedSwitchCases& generated_switch_cases,
    DexMethod* method,
    DexType* enum_type,
    DexField* lookup_table) {

  auto generated_cls = type_class(method->get_class());
  optimize_enums::OptimizeEnumsGeneratedAnalysis analysis(generated_cls,
                                                          enum_type);
  analysis.collect_generated_switch_cases(generated_switch_cases);
}

/**
 * Get `java.lang.Enum`'s ctor.
 * Details: https://developer.android.com/reference/java/lang/Enum.html
 */
DexMethod* get_java_enum_ctor() {
  DexType* java_enum_type = get_enum_type();
  DexClass* java_enum_cls = type_class(java_enum_type);
  const std::vector<DexMethod*>& java_enum_ctors = java_enum_cls->get_ctors();

  always_assert(java_enum_ctors.size() == 1);
  return java_enum_ctors.at(0);
}

/**
 * Based on https://docs.oracle.com/javase/7/docs/api/java/lang/Enum.html,
 * when we replace enums with Integer object, we consider the enums that
 * only have following method invocations are safe.
 *
 *   INVOKE_DIRECT <init>:(Ljava/lang/String;I)V // <init> may be override
 *   INVOKE_VIRTUAL equals:(Ljava/lang/Object;)Z
 *   INVOKE_VIRTUAL compareTo:(Ljava/lang/Enum;)I
 *   INVOKE_VIRTUAL ordinal:()I
 *   INVOKE_VIRTUAL toString:()Ljava/lang/String;
 *   INVOKE_VIRTUAL name:()Ljava/lang/String;
 *   INVOKE_STATIC values:()[LE;
 *   INVOKE_STATIC valueOf:(Ljava/lang/String;)LE;
 *   TODO(fengliu): hashCode ?
 * Unsafe methods :
 *   user defined constructors
 */
std::unordered_map<DexString*, DexProto*> get_safe_enum_methods() {
  std::unordered_map<DexString*, DexProto*> methods;
  // It's Okay if keys and values are nullptr
  methods[DexString::get_string("<init>")] = DexProto::make_proto(
      get_void_type(),
      DexTypeList::make_type_list({get_string_type(), get_int_type()}));
  methods[DexString::get_string("equals")] = DexProto::make_proto(
      get_boolean_type(), DexTypeList::make_type_list({get_object_type()}));
  methods[DexString::get_string("compareTo")] = DexProto::make_proto(
      get_int_type(), DexTypeList::make_type_list({get_enum_type()}));
  methods[DexString::get_string("ordinal")] =
      DexProto::make_proto(get_int_type(), DexTypeList::make_type_list({}));
  methods[DexString::get_string("toString")] =
      DexProto::make_proto(get_string_type(), DexTypeList::make_type_list({}));
  methods[DexString::get_string("name")] =
      DexProto::make_proto(get_string_type(), DexTypeList::make_type_list({}));
  // values() and valueOf(String) are considered separately.
  return methods;
}

class OptimizeEnums {
 public:
  OptimizeEnums(DexStoresVector& stores, ConfigFiles& conf)
      : m_stores(stores), m_pg_map(conf.get_proguard_map()) {
    m_scope = build_class_scope(stores);
    m_java_enum_ctor = get_java_enum_ctor();
  }

  void remove_redundant_generated_classes() {
    auto generated_classes = collect_generated_classes();
    auto enum_field_to_ordinal = collect_enum_field_ordinals();

    std::unordered_set<DexType*> collected_enums;
    for (const auto& pair : enum_field_to_ordinal) {
      collected_enums.emplace(pair.first->get_class());
    }

    std::unordered_map<DexField*, DexType*> lookup_table_to_enum;
    GeneratedSwitchCases generated_switch_cases;

    for (const auto& generated_cls : generated_classes) {
      auto generated_clinit = generated_cls->get_clinit();

      for (const auto& sfield : generated_cls->get_sfields()) {
        // update stats.
        m_stats.num_lookup_tables++;

        auto enum_type = get_enum_used(sfield);
        if (!enum_type || collected_enums.count(enum_type) == 0) {
          // Nothing to do if we couldn't determine enum ordinals.
          continue;
        }

        lookup_table_to_enum[sfield] = enum_type;
        collect_generated_switch_cases(generated_switch_cases, generated_clinit,
                                       enum_type, sfield);
      }
    }

    remove_generated_classes_usage(lookup_table_to_enum, enum_field_to_ordinal,
                                   generated_switch_cases);
  }

  void stats(PassManager& mgr) {
    mgr.set_metric(METRIC_NUM_SYNTHETIC_CLASSES, m_stats.num_synthetic_classes);
    mgr.set_metric(METRIC_NUM_LOOKUP_TABLES, m_stats.num_lookup_tables);
    mgr.set_metric(METRIC_NUM_LOOKUP_TABLES_REMOVED,
                   m_lookup_tables_replaced.size());
    mgr.set_metric(METRIC_NUM_ENUM_CLASSES, m_stats.num_enum_classes);
    mgr.set_metric(METRIC_NUM_ENUM_OBJS, m_stats.num_enum_objs);
    mgr.set_metric(METRIC_NUM_INT_OBJS, m_stats.num_int_objs);
  }

  /**
   * Replace enum with Boxed Integer object
   */
  void replace_enum_with_int() {
    const ConcurrentSet<DexType*>& candidate_enums = collect_simple_enums();
    TRACE(ENUM, 1, "\tCandidate enum classes : %d\n", candidate_enums.size());
    m_stats.num_enum_objs = optimize_enums::transform_enums(
        candidate_enums, &m_stores, &m_stats.num_int_objs);
    TRACE(ENUM, 1, "\tTransformed enum objects : %d\n", m_stats.num_enum_objs);
    TRACE(ENUM, 1, "\tGenerated integer object : %d\n", m_stats.num_int_objs);
    m_stats.num_enum_classes = candidate_enums.size();
  }

 private:
  /**
   * Reject enums which are invoked through unsupported method.
   */
  void reject_unsafe_invocation(ConcurrentSet<DexType*>* enum_set) {
    const std::unordered_map<DexString*, DexProto*> safe_methods =
        get_safe_enum_methods();
    // invoke-static LE;.values:()[LE; is not allowed in the first version
    const DexString* values_method = DexString::get_string("values");
    // invoke-static LE;.valueOf(Ljava / lang / String;) LE; is not allowed
    const DexString* valueof_method = DexString::get_string("valueOf");

    ConcurrentSet<DexType*> rejected_enums;

    walk::parallel::methods(m_scope, [&](DexMethod* method) {
      if (!method->get_code()) {
        return;
      }
      for (const auto& mie : InstructionIterable(method->get_code())) {
        if (mie.insn->has_method()) {
          DexMethodRef* called_method_ref = mie.insn->get_method();
          DexType* clazz = called_method_ref->get_class();
          DexProto* proto = called_method_ref->get_proto();
          // If the invoked method is from candidate enums, detect if the
          // invocation is allowed or not
          if (clazz != nullptr && enum_set->count(clazz) &&
              !rejected_enums.count(clazz)) {
            if (safe_methods.count(called_method_ref->get_name())) {
              if (proto != safe_methods.at(called_method_ref->get_name())) {
                rejected_enums.insert(clazz);
              }
            } else if (called_method_ref->get_name() == values_method) {
              // Only values:()[LE; is acceptable.
              if (proto->get_args()->size()) {
                rejected_enums.insert(clazz);
              }
            } else if (mie.insn->opcode() != OPCODE_INVOKE_STATIC) {
              // None static invocation on enums are not allowed.
              rejected_enums.insert(clazz);
            }
            // Invocation on static methods are allowed.
          }
        }
      }
    });
    for (DexType* type : rejected_enums) {
      enum_set->erase(type);
    }
  }

  ConcurrentSet<DexType*> collect_simple_enums() {
    ConcurrentSet<DexType*> enum_set;
    walk::classes(m_scope, [&](DexClass* cls) {
      if (is_simple_enum(cls)) {
        enum_set.insert(cls->get_type());
      }
    });
    reject_unsafe_invocation(&enum_set);
    optimize_enums::reject_unsafe_enums(m_scope, &enum_set);
    return enum_set;
  }

  /**
   * TODO(fengliu) : Some enums with cached values should be optimized.
   * But we simply ignore them in the first version.
   */
  static bool is_simple_enum(const DexClass* cls) {
    if (!is_enum(cls) || cls->is_external() || !is_final(cls) ||
        !can_delete(cls) || cls->get_interfaces()->size() > 0 ||
        !cls->get_ifields().empty() || !cls->get_vmethods().empty()) {
      return false;
    }
    // Examine the constructors' instructions.
    for (const DexMethod* method : cls->get_dmethods()) {
      if (is_init(method) && !is_simple_enum_constructor(method)) {
        return false;
      }
    }

    // No other kinds of static fields
    const DexType* array_of_enum = make_array_type(cls->get_type());
    always_assert(array_of_enum != nullptr);
    const DexString* values_str = DexString::get_string("$VALUES");
    for (const DexField* field : cls->get_sfields()) {
      if (field->get_type() == array_of_enum) {
        if (field->get_name() != values_str) {
          return false;
        }
      } else if (field->get_type() != cls->get_type()) {
        return false;
      }
    }
    return true;
  }

  /**
   * Detect if a constructor is a simple enum constructor.
   * A simple enum constructor is
   *  load_param * // multiple load parameter instructions
   *  invoke-direct {} Ljava/lang/Enum;.<init>:(Ljava/lang/String;I)V
   *  return-void
   */
  static bool is_simple_enum_constructor(const DexMethod* method) {
    const auto& args = method->get_proto()->get_args()->get_type_list();
    if (args.size() < 2) {
      return false;
    }
    // First two arguments should always be string and int.
    {
      auto it = args.begin();
      if (*it != get_string_type()) {
        return false;
      }
      if (*(++it) != get_int_type()) {
        return false;
      }
    }
    auto code = InstructionIterable(method->get_code());
    auto it = code.begin();
    // Load parameter instructions.
    while (it != code.end() && opcode::is_load_param(it->insn->opcode())) {
      ++it;
    }
    if (it == code.end()) {
      return false;
    }
    // invoke-direct {} Ljava/lang/Enum;.<init>:(Ljava/lang/String;I)V
    if (!is_invoke_direct(it->insn->opcode())) {
      return false;
    } else {
      const DexMethodRef* ref = it->insn->get_method();
      // Enum.<init>
      if (ref->get_class() != get_enum_type() || !is_constructor(ref)) {
        return false;
      }
    }
    if (++it == code.end()) {
      return false;
    }
    // return-void
    if (!is_return_void(it->insn->opcode())) {
      return false;
    }
    // No more instructions.
    return (++it) == code.end();
  }

  /**
   * We determine which classes are generated based on:
   * - classes that only have 1 dmethods: <clinit>
   * - no instance fields, nor virtual methods
   * - all static fields match `$SwitchMap$<enum_path>`
   */
  std::vector<DexClass*> collect_generated_classes() {
    std::vector<DexClass*> generated_classes;
    std::unordered_set<DexClass*> scope_classes(m_scope.begin(), m_scope.end());

    // To avoid any cross store references, only accept generated classes
    // that are in the root store (same for the Enums they reference).
    XStoreRefs xstores(m_stores);

    for (const auto& cls : m_scope) {
      size_t cls_store_idx = xstores.get_store_idx(cls->get_type());
      if (cls_store_idx > 1) {
        continue;
      }

      auto& sfields = cls->get_sfields();

      // We expect the generated classes to ONLY contain the lookup tables
      // and the static initializer (<clinit>)
      if (sfields.size() > 0 && cls->get_dmethods().size() == 1 &&
          cls->get_vmethods().size() == 0 && cls->get_ifields().size() == 0) {

        bool accept_cls = true;
        for (auto sfield : sfields) {
          const std::string& deobfuscated_name =
              sfield->get_deobfuscated_name();
          const std::string& sfield_name = deobfuscated_name.empty()
                                               ? sfield->get_name()->str()
                                               : deobfuscated_name;

          if (sfield_name.find("$SwitchMap$") == std::string::npos) {
            accept_cls = false;
            break;
          }
        }

        if (accept_cls) {
          generated_classes.emplace_back(cls);
        }
      }
    }

    // Update stats.
    m_stats.num_synthetic_classes = generated_classes.size();

    return generated_classes;
  }

  std::unordered_map<DexField*, size_t> collect_enum_field_ordinals() {
    std::unordered_map<DexField*, size_t> enum_field_to_ordinal;

    for (const auto& cls : m_scope) {
      if (is_enum(cls)) {
        collect_enum_field_ordinals(cls, enum_field_to_ordinal);
      }
    }

    return enum_field_to_ordinal;
  }

  /**
   * Collect enum fields to ordinal, if <clinit> is defined.
   */
  void collect_enum_field_ordinals(
      const DexClass* cls,
      std::unordered_map<DexField*, size_t>& enum_field_to_ordinal) {
    if (!cls) {
      return;
    }

    auto clinit = cls->get_clinit();
    if (!clinit || !clinit->get_code()) {
      return;
    }

    std::unordered_map<const DexMethod*, uint32_t> ctor_to_arg_ordinal;
    if (!analyze_enum_ctors(cls, m_java_enum_ctor, ctor_to_arg_ordinal)) {
      return;
    }

    optimize_enums::OptimizeEnumsAnalysis analysis(cls, ctor_to_arg_ordinal);
    analysis.collect_ordinals(enum_field_to_ordinal);
  }

  /**
   * Removes the usage of the generated lookup table, by rewriting switch cases
   * based on enum ordinals.
   *
   * The initial switch looks like:
   *
   * switch (enum_element) {
   *  case enum_0:
   *    // do something
   *  case enum_7:
   *    // do something
   * }
   *
   * which was re-written to:
   *
   * switch (int_element) {
   *  case 1:
   *    // do something for enum_0
   *  case 2:
   *    // do something for enum_7
   * }
   *
   * which we are changing to:
   *
   * switch (ordinal_element) {
   *  case 0:
   *    // do something for enum_0
   *  case 7:
   *    // do something for enum_7
   * }
   */
  void remove_generated_classes_usage(
      const std::unordered_map<DexField*, DexType*>& lookup_table_to_enum,
      const std::unordered_map<DexField*, size_t>& enum_field_to_ordinal,
      const GeneratedSwitchCases& generated_switch_cases) {

    namespace cp = constant_propagation;
    walk::code(m_scope, [&](DexMethod*, IRCode& code) {
      code.build_cfg(/* editable */ true);
      auto& cfg = code.cfg();
      cfg.calculate_exit_block();
      optimize_enums::Iterator fixpoint(&cfg);
      fixpoint.run(optimize_enums::Environment());
      std::unordered_set<IRInstruction*> switches;
      for (const auto& info : fixpoint.collect()) {
        const auto pair = switches.insert((*info.branch)->insn);
        bool insert_occurred = pair.second;
        if (!insert_occurred) {
          // Make sure we don't have any duplicate switch opcodes. We can't
          // change the register of a switch opcode to two different registers.
          continue;
        }
        if (!check_lookup_table_usage(lookup_table_to_enum,
                                      enum_field_to_ordinal,
                                      generated_switch_cases, info)) {
          continue;
        }
        remove_lookup_table_usage(enum_field_to_ordinal, generated_switch_cases,
                                  info);
      }
      code.clear_cfg();
    });
  }

  /**
   * Check to make sure this is a valid match. Return false to abort the
   * optimization.
   */
  bool check_lookup_table_usage(
      const std::unordered_map<DexField*, DexType*>& lookup_table_to_enum,
      const std::unordered_map<DexField*, size_t>& enum_field_to_ordinal,
      const GeneratedSwitchCases& generated_switch_cases,
      const optimize_enums::Info& info) {

    // Check this is called on an enum.
    auto invoke_ordinal = (*info.invoke)->insn;
    auto invoke_type = invoke_ordinal->get_method()->get_class();
    auto invoke_cls = type_class(invoke_type);
    if (!invoke_cls || !is_enum(invoke_cls)) {
      return false;
    }

    auto lookup_table = info.array_field;
    if (!lookup_table || lookup_table_to_enum.count(lookup_table) == 0) {
      return false;
    }

    // Check the current enum corresponds.
    auto current_enum = lookup_table_to_enum.at(lookup_table);
    if (current_enum != invoke_type) {
      return false;
    }
    return true;
  }

  /**
   * Replaces the usage of the lookup table, by converting
   *
   * INVOKE_VIRTUAL <v_enum> Enum;.ordinal:()
   * MOVE_RESULT <v_ordinal>
   * AGET <v_field>, <v_ordinal>
   * MOVE_RESULT_PSEUDO <v_dest>
   * *_SWITCH <v_dest>              ; or IF_EQZ <v_dest> <v_some_constant>
   *
   * to
   *
   * INVOKE_VIRTUAL <v_enum> Enum;.ordinal:()
   * MOVE_RESULT <v_ordinal>
   * MOVE <v_dest>, <v_ordinal>
   * SPARSE_SWITCH <v_dest>
   *
   * if <v_field> was fetched using SGET_OBJECT <lookup_table_holder>
   *
   * and updating switch cases (on the edges) to the enum field's ordinal
   *
   * NOTE: We leave unused code around, since LDCE should remove it
   *       if it isn't used afterwards (which is expected), but we are
   *       being conservative.
   */
  void remove_lookup_table_usage(
      const std::unordered_map<DexField*, size_t>& enum_field_to_ordinal,
      const GeneratedSwitchCases& generated_switch_cases,
      const optimize_enums::Info& info) {
    auto& cfg = info.branch->cfg();
    auto branch_block = info.branch->block();

    // Use the SwitchEquivFinder to handle not just switch statements but also
    // trees of if and switch statements
    SwitchEquivFinder finder(&cfg, *info.branch, *info.reg);
    if (!finder.success()) {
      return;
    }

    // Remove the switch statement so we can rebuild it with the correct case
    // keys. This removes all edges to the if-else blocks and the blocks will
    // eventually be removed by cfg.simplify()
    cfg.remove_insn(*info.branch);

    cfg::Block* fallthrough = nullptr;
    std::vector<std::pair<int32_t, cfg::Block*>> cases;
    const auto& field_enum_map = generated_switch_cases.at(info.array_field);
    for (const auto& pair : finder.key_to_case()) {
      auto old_case_key = pair.first;
      cfg::Block* leaf = pair.second;

      // if-else chains will load constants to compare against. Sometimes the
      // leaves will use these values so we have to copy those values to the
      // beginning of the leaf blocks. Any dead instructions will be cleaned up
      // by LDCE.
      const auto& extra_loads = finder.extra_loads();
      const auto& loads_for_this_leaf = extra_loads.find(leaf);
      if (loads_for_this_leaf != extra_loads.end()) {
        for (const auto& register_and_insn : loads_for_this_leaf->second) {
          IRInstruction* insn = register_and_insn.second;
          if (insn != nullptr) {
            // null instruction pointers are used to signify the upper half of a
            // wide load.
            auto copy = new IRInstruction(*insn);
            TRACE(ENUM, 4, "adding %s to B%d\n", SHOW(copy), leaf->id());
            leaf->push_front(copy);
          }
        }
      }

      if (old_case_key == boost::none) {
        always_assert_log(fallthrough == nullptr, "only 1 fallthrough allowed");
        fallthrough = leaf;
        continue;
      }

      auto search = field_enum_map.find(*old_case_key);
      always_assert_log(search != field_enum_map.end(),
                        "can't find case key %d leaving block %d\n%s\nin %s\n",
                        *old_case_key, branch_block->id(), info.str().c_str(),
                        SHOW(cfg));
      auto field_enum = search->second;
      auto new_case_key = enum_field_to_ordinal.at(field_enum);
      cases.emplace_back(new_case_key, leaf);
    }

    // Add a new register to hold the ordinal and then use it to
    // switch on the actual ordinal, instead of using the lookup table.
    //
    // Basically, the bytecode will be:
    //
    //  INVOKE_VIRTUAL <v_enum> <Enum>;.ordinal:()
    //  MOVE_RESULT <v_ordinal>
    //  MOVE <v_new_reg> <v_ordinal> // Newly added
    //  ...
    //  AGET <v_field>, <v_ordinal>
    //  MOVE_RESULT_PSEUDO <v_dest>
    //  ...
    //  SPARSE_SWITCH <v_new_reg> // will use <v_new_reg> instead of <v_dest>
    //
    // NOTE: We leave CopyPropagation to clean up the extra moves and
    //       LDCE the array access.
    auto move_ordinal_it = cfg.move_result_of(*info.invoke);
    auto move_ordinal = move_ordinal_it->insn;
    auto reg_ordinal = move_ordinal->dest();
    auto new_ordinal_reg = cfg.allocate_temp();
    auto move_ordinal_result = new IRInstruction(OPCODE_MOVE);
    move_ordinal_result->set_src(0, reg_ordinal);
    move_ordinal_result->set_dest(new_ordinal_reg);
    cfg.insert_after(move_ordinal_it, move_ordinal_result);

    // TODO?: Would it be possible to keep the original if-else tree form that
    // D8 made?  It would probably be better to build a more general purpose
    // switch -> if-else converter pass and run it after this pass.
    if (cases.size() > 1) {
      // Dex Lowering will convert this to packed if that would be better
      IRInstruction* new_switch = new IRInstruction(OPCODE_SPARSE_SWITCH);
      new_switch->set_src(0, new_ordinal_reg);
      cfg.create_branch(branch_block, new_switch, fallthrough, cases);
    } else if (cases.size() == 1) {
      // Only one non-fallthrough case, we can use an if statement.
      // const vKey, case_key
      // if-eqz vOrdinal, vKey
      int32_t key = cases[0].first;
      cfg::Block* target = cases[0].second;
      IRInstruction* const_load = new IRInstruction(OPCODE_CONST);
      auto key_reg = cfg.allocate_temp();
      const_load->set_dest(key_reg);
      const_load->set_literal(key);
      branch_block->push_back(const_load);

      IRInstruction* new_if = new IRInstruction(OPCODE_IF_EQ);
      new_if->set_src(0, new_ordinal_reg);
      new_if->set_src(1, key_reg);
      cfg.create_branch(branch_block, new_if, fallthrough, target);
    } else {
      // Just one case, set the goto edge's target to the fallthrough block.
      always_assert(cases.empty());
      if (fallthrough != nullptr) {
        auto existing_goto =
            cfg.get_succ_edge_of_type(branch_block, cfg::EDGE_GOTO);
        always_assert(existing_goto != nullptr);
        cfg.set_edge_target(existing_goto, fallthrough);
      }
    }

    m_lookup_tables_replaced.emplace(info.array_field);
  }

  /**
   * Generated field names follow the format:
   *   $SwitchMap$com$<part_of_path_1>$...$<enum_name>
   * where Lcom/<part_of_path_1>/.../enum_name; is the actual enum.
   */
  DexType* get_enum_used(DexField* field) {
    const auto& deobfuscated_name = field->get_deobfuscated_name();
    const auto& name = deobfuscated_name.empty() ? field->get_name()->str()
                                                 : deobfuscated_name;

    // Get the class path, by removing the first part of the field
    // name ($SwitchMap$), adding 'L' and ';' and replacing '$' with '/'.
    auto pos = name.find("SwitchMap");
    always_assert(pos != std::string::npos);
    auto start_index = pos + 10;

    auto end = name.find(":[");
    if (end == std::string::npos) {
      end = name.size();
    }

    std::string class_name =
        "L" + name.substr(start_index, end - start_index) + ";";
    std::replace(class_name.begin(), class_name.end(), '$', '/');

    // We search for the enum type recursively. If the initial path doesn't
    // correspond to an enum, we check if it is an inner class.
    DexType* type = nullptr;
    do {
      type = DexType::get_type(class_name.c_str());

      if (!type && !m_pg_map.empty()) {
        const std::string& obfuscated_name =
            m_pg_map.translate_class(class_name.c_str());

        // Get type associated type from the obfuscated class name.
        if (!obfuscated_name.empty()) {
          type = DexType::get_type(obfuscated_name.c_str());
        }
      }

      if (type && is_enum(type_class(type))) {
        return type;
      }

      std::size_t found = class_name.find_last_of("/");
      if (found == std::string::npos) {
        break;
      }

      class_name[found] = '$';
    } while (true);

    return nullptr;
  }

  Scope m_scope;
  DexStoresVector& m_stores;

  struct Stats {
    size_t num_synthetic_classes{0};
    size_t num_lookup_tables{0};
    size_t num_enum_classes{0};
    size_t num_enum_objs{0};
    size_t num_int_objs{0};
  };
  Stats m_stats;

  std::unordered_set<DexField*> m_lookup_tables_replaced;
  const DexMethod* m_java_enum_ctor;
  const ProguardMap& m_pg_map;
};

} // namespace

namespace optimize_enums {

void OptimizeEnumsPass::run_pass(DexStoresVector& stores,
                                 ConfigFiles& conf,
                                 PassManager& mgr) {
  OptimizeEnums opt_enums(stores, conf);
  opt_enums.remove_redundant_generated_classes();
  opt_enums.replace_enum_with_int();
  opt_enums.stats(mgr);
}

static OptimizeEnumsPass s_pass;

} // namespace optimize_enums
