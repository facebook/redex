/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OptimizeEnums.h"

#include "ClassAssemblingUtils.h"
#include "ConfigFiles.h"
#include "EnumAnalyzeGeneratedMethods.h"
#include "EnumClinitAnalysis.h"
#include "EnumInSwitch.h"
#include "EnumTransformer.h"
#include "EnumUpcastAnalysis.h"
#include "IRCode.h"
#include "MatchFlow.h"
#include "OptimizeEnumsAnalysis.h"
#include "PassManager.h"
#include "ProguardMap.h"
#include "Resolver.h"
#include "ScopedCFG.h"
#include "SwitchEquivFinder.h"
#include "Trace.h"
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

// Map the field holding the lookup table to its associated enum type.
using LookupTableToEnum = std::unordered_map<DexField*, DexType*>;

// Map the static fields holding enumerands to their ordinal number (passed in
// to their constructor).
using EnumFieldToOrdinal = std::unordered_map<DexField*, size_t>;

// Sets of types.  Intended to be sub-classes of Ljava/lang/Enum; but not
// guaranteed by the type.
using EnumTypes = std::unordered_set<DexType*>;

// Lookup tables in generated classes map enum ordinals to the integers they
// are represented by in switch statements using that lookup table:
//
//   lookup[enum.ordinal()] = case;
//
// GeneratedSwitchCases represent the reverse mapping for a lookup table:
//
//   gsc[lookup][case] = enum
//
// with lookup and enum identified by their fields.
using GeneratedSwitchCases =
    std::unordered_map<DexField*, std::unordered_map<size_t, DexField*>>;

constexpr const char* METRIC_NUM_SYNTHETIC_CLASSES = "num_synthetic_classes";
constexpr const char* METRIC_NUM_LOOKUP_TABLES = "num_lookup_tables";
constexpr const char* METRIC_NUM_LOOKUP_TABLES_REMOVED =
    "num_lookup_tables_replaced";
constexpr const char* METRIC_NUM_ENUM_CLASSES = "num_candidate_enum_classes";
constexpr const char* METRIC_NUM_ENUM_OBJS = "num_erased_enum_objs";
constexpr const char* METRIC_NUM_INT_OBJS = "num_generated_int_objs";
constexpr const char* METRIC_NUM_SWITCH_EQUIV_FINDER_FAILURES =
    "num_switch_equiv_finder_failures";
constexpr const char* METRIC_NUM_CANDIDATE_GENERATED_METHODS =
    "num_candidate_generated_enum_methods";
constexpr const char* METRIC_NUM_REMOVED_GENERATED_METHODS =
    "num_removed_generated_enum_methods";

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

  struct DelegatingCall {
    DexMethod* ctor;
    cfg::ScopedCFG cfg;
    IRInstruction* invoke;

    DelegatingCall(DexMethod* ctor, cfg::ScopedCFG cfg, IRInstruction* invoke)
        : ctor{ctor}, cfg{std::move(cfg)}, invoke{invoke} {}
  };

  std::queue<DelegatingCall> delegating_calls;
  { // Find delegate constructor calls and queue them up to be processed.  The
    // call might be to `Enum.<init>(String;I)` or to a difference constructor
    // of the same class.
    mf::flow_t f;
    auto inv = f.insn(m::invoke_direct_(m::has_method(m::resolve_method(
        MethodSearch::Direct,
        m::equals(java_enum_ctor) ||
            m::is_constructor<DexMethod>() &&
                m::member_of<DexMethod>(m::equals(cls->get_type()))))));

    for (const auto& ctor : cls->get_ctors()) {
      auto code = ctor->get_code();
      if (!code) {
        return false;
      }

      cfg::ScopedCFG cfg{code};
      auto res = f.find(*cfg, inv);
      if (auto* inv_insn = res.matching(inv).unique()) {
        delegating_calls.emplace(ctor, std::move(cfg), inv_insn);
      } else {
        return false;
      }
    }
  }

  // Ordinal represents the third argument.
  // details: https://developer.android.com/reference/java/lang/Enum.html
  ctor_to_arg_ordinal[java_enum_ctor] = 2;

  // TODO: We could order them instead of looping ...
  for (; !delegating_calls.empty(); delegating_calls.pop()) {
    auto dc = std::move(delegating_calls.front());

    auto* delegate =
        resolve_method(dc.invoke->get_method(), MethodSearch::Direct);

    uint32_t delegate_ordinal;
    { // Only proceed if the delegate constructor has already been processed.
      auto it = ctor_to_arg_ordinal.find(delegate);
      if (it == ctor_to_arg_ordinal.end()) {
        delegating_calls.emplace(std::move(dc));
        continue;
      } else {
        delegate_ordinal = it->second;
      }
    }

    // Track which param in dc.ctor flows into the ordinal arg of the delegate.
    mf::flow_t f;
    auto param = f.insn(m::load_param_());
    auto invoke_delegate =
        f.insn(m::equals(dc.invoke))
            .src(delegate_ordinal, param, mf::unique | mf::alias);

    auto res = f.find(*dc.cfg, invoke_delegate);

    auto* load_ordinal = res.matching(param).unique();
    if (!load_ordinal) {
      // Couldn't find a unique parameter flowing into the ordinal argument.
      return false;
    }

    // Figure out which param is being loaded.
    uint32_t ctor_ordinal = 0;
    auto ii = InstructionIterable(dc.cfg->get_param_instructions());
    for (auto it = ii.begin(), end = ii.end();; ++it, ++ctor_ordinal) {
      always_assert(it != end && "Unable to locate load_ordinal");
      if (it->insn == load_ordinal) break;
    }

    ctor_to_arg_ordinal[dc.ctor] = ctor_ordinal;
  }

  return true;
}

/**
 * Discover the mapping from enums to cases in lookup tables defined on
 * `generated_cls` by detecting the following patterns in its `<clinit>` (modulo
 * ordering and interleaved unrelated instructions):
 *
 *   sget-object               <lookup>
 *   move-result-pseudo-object v0
 *
 * Or:
 *
 *   new-array                 ..., [I
 *   move-result-pseudo-object v0
 *   sput-object               v0, <lookup>
 *
 * Followed by:
 *
 *   sget-object               <enum>
 *   move-result-pseudo-object v1
 *   invoke-virtual            {v1}, Ljava/lang/Enum;.ordinal:()I
 *   move-result               v2
 *   const                     v3, <kase>
 *   aput                      v3, v0, v2
 *
 * For each instance of the pattern found, a `generated_switch_cases` entry is
 * added:
 *
 *   generated_switch_cases[lookup][kase] = enum;
 */
void collect_generated_switch_cases(
    DexClass* generated_cls,
    cfg::ControlFlowGraph& clinit_cfg,
    const EnumTypes& collected_enums,
    GeneratedSwitchCases& generated_switch_cases) {
  mf::flow_t f;

  DexMethod* Enum_ordinal =
      resolve_method(DexMethod::get_method("Ljava/lang/Enum;.ordinal:()I"),
                     MethodSearch::Virtual);
  always_assert(Enum_ordinal);

  auto m__generated_field = m::has_field(
      m::member_of<DexFieldRef>(m::equals(generated_cls->get_type())));
  auto m__lookup = m::sget_object_(m__generated_field) || m::new_array_();
  auto m__sget_enum = m::sget_object_(m::has_field(
      m::member_of<DexFieldRef>(m::in<DexType*>(collected_enums))));
  auto m__invoke_ordinal = m::invoke_virtual_(m::has_method(
      m::resolve_method(MethodSearch::Virtual, m::equals(Enum_ordinal))));

  auto uniq = mf::alias | mf::unique;
  auto look = f.insn(m__lookup);
  auto gete = f.insn(m__sget_enum);
  auto kase = f.insn(m::const_());
  auto ordi = f.insn(m__invoke_ordinal).src(0, gete, uniq);
  auto aput = f.insn(m::aput_())
                  .src(0, kase, uniq)
                  .src(1, look, uniq)
                  .src(2, ordi, uniq);

  auto res = f.find(clinit_cfg, aput);

  std::unordered_map<IRInstruction*, IRInstruction*> new_array_to_sput;
  for (auto* insn_look : res.matching(look)) {
    if (opcode::is_new_array(insn_look->opcode())) {
      new_array_to_sput.emplace(insn_look, nullptr);
    }
  }

  // Some lookup tables are accessed fresh rather than via an sget-object, so
  // look at where the new arrays are put to determine the field.
  if (!new_array_to_sput.empty()) {
    mf::flow_t g;

    auto m__sput_lookup = m::sput_object_(m__generated_field);

    auto newa = g.insn(m::in<IRInstruction*>(new_array_to_sput));
    auto sput = g.insn(m__sput_lookup).src(0, newa, uniq);

    auto res_sputs = g.find(clinit_cfg, sput);
    for (auto* insn_sput : res_sputs.matching(sput)) {
      auto* insn_newa = res_sputs.matching(sput, insn_sput, 0).unique();
      new_array_to_sput[insn_newa] = insn_sput;
    }
  }

  for (auto* insn_aput : res.matching(aput)) {
    auto* insn_kase = res.matching(aput, insn_aput, 0).unique();
    auto* insn_look = res.matching(aput, insn_aput, 1).unique();
    auto* insn_ordi = res.matching(aput, insn_aput, 2).unique();
    auto* insn_gete = res.matching(ordi, insn_ordi, 0).unique();

    if (opcode::is_new_array(insn_look->opcode())) {
      // If the array being assigned to came from a new-array, look for the sput
      // it flowed into.
      insn_look = new_array_to_sput.at(insn_look);
    }

    auto switch_case = insn_kase->get_literal();
    auto* lookup_table =
        resolve_field(insn_look->get_field(), FieldSearch::Static);
    auto* enum_field =
        resolve_field(insn_gete->get_field(), FieldSearch::Static);

    always_assert(lookup_table);
    always_assert(enum_field && is_enum(enum_field));
    always_assert_log(switch_case > 0,
                      "The generated SwitchMap should have positive keys");

    generated_switch_cases[lookup_table].emplace(switch_case, enum_field);
  }
}

/**
 * Get `java.lang.Enum`'s ctor.
 * Details: https://developer.android.com/reference/java/lang/Enum.html
 */
DexMethod* get_java_enum_ctor() {
  DexType* java_enum_type = type::java_lang_Enum();
  DexClass* java_enum_cls = type_class(java_enum_type);
  const std::vector<DexMethod*>& java_enum_ctors = java_enum_cls->get_ctors();

  always_assert(java_enum_ctors.size() == 1);
  return java_enum_ctors.at(0);
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

    EnumTypes collected_enums;
    for (const auto& pair : enum_field_to_ordinal) {
      collected_enums.emplace(pair.first->get_class());
    }

    LookupTableToEnum lookup_table_to_enum;
    GeneratedSwitchCases generated_switch_cases;

    for (const auto& generated_cls : generated_classes) {
      auto generated_clinit = generated_cls->get_clinit();
      cfg::ScopedCFG clinit_cfg{generated_clinit->get_code()};

      associate_lookup_tables_to_enums(generated_cls, *clinit_cfg,
                                       collected_enums, lookup_table_to_enum);
      collect_generated_switch_cases(generated_cls, *clinit_cfg,
                                     collected_enums, generated_switch_cases);

      // update stats.
      m_stats.num_lookup_tables += generated_cls->get_sfields().size();
    }

    remove_generated_classes_usage(lookup_table_to_enum, enum_field_to_ordinal,
                                   generated_switch_cases);
  }

  void stats(PassManager& mgr) {
    const auto& report = [&mgr](const char* name, size_t stat) {
      mgr.set_metric(name, stat);
      TRACE(ENUM, 1, "\t%s : %zu", name, stat);
    };
    report(METRIC_NUM_SYNTHETIC_CLASSES, m_stats.num_synthetic_classes);
    report(METRIC_NUM_LOOKUP_TABLES, m_stats.num_lookup_tables);
    report(METRIC_NUM_LOOKUP_TABLES_REMOVED, m_lookup_tables_replaced.size());
    report(METRIC_NUM_ENUM_CLASSES, m_stats.num_enum_classes);
    report(METRIC_NUM_ENUM_OBJS, m_stats.num_enum_objs);
    report(METRIC_NUM_INT_OBJS, m_stats.num_int_objs);
    report(METRIC_NUM_SWITCH_EQUIV_FINDER_FAILURES,
           m_stats.num_switch_equiv_finder_failures);
    report(METRIC_NUM_CANDIDATE_GENERATED_METHODS,
           m_stats.num_candidate_generated_methods);
    report(METRIC_NUM_REMOVED_GENERATED_METHODS,
           m_stats.num_removed_generated_methods);
  }

  /**
   * Replace enum with Boxed Integer object
   */
  void replace_enum_with_int(int max_enum_size,
                             const std::vector<DexType*>& allowlist) {
    if (max_enum_size <= 0) {
      return;
    }
    optimize_enums::Config config(max_enum_size, allowlist);
    const auto override_graph = method_override_graph::build_graph(m_scope);
    calculate_param_summaries(m_scope, *override_graph,
                              &config.param_summary_map);

    /**
     * An enum is safe if it not external, has no interfaces, and has only one
     * simple enum constructor. Static fields, primitive or string instance
     * fields, and virtual methods are safe.
     */
    auto is_safe_enum = [this](const DexClass* cls) {
      if (is_enum(cls) && !cls->is_external() && is_final(cls) &&
          can_delete(cls) && cls->get_interfaces()->size() == 0 &&
          only_one_static_synth_field(cls)) {

        const auto& ctors = cls->get_ctors();
        if (ctors.size() != 1 ||
            !is_simple_enum_constructor(cls, ctors.front())) {
          return false;
        }

        for (auto& dmethod : cls->get_dmethods()) {
          if (is_static(dmethod) || method::is_constructor(dmethod)) {
            continue;
          }
          if (!can_rename(dmethod)) {
            return false;
          }
        }

        for (auto& vmethod : cls->get_vmethods()) {
          if (!can_rename(vmethod)) {
            return false;
          }
        }

        const auto& ifields = cls->get_ifields();
        return std::all_of(ifields.begin(), ifields.end(), [](DexField* field) {
          auto type = field->get_type();
          return type::is_primitive(type) || type == type::java_lang_String();
        });
      }
      return false;
    };

    walk::parallel::classes(m_scope, [&config, is_safe_enum](DexClass* cls) {
      if (is_safe_enum(cls)) {
        config.candidate_enums.insert(cls->get_type());
      }
    });

    optimize_enums::reject_unsafe_enums(m_scope, &config);
    if (traceEnabled(ENUM, 4)) {
      for (auto cls : config.candidate_enums) {
        TRACE(ENUM, 4, "candidate_enum %s", SHOW(cls));
      }
    }
    m_stats.num_enum_objs = optimize_enums::transform_enums(
        config, &m_stores, &m_stats.num_int_objs);
    m_stats.num_enum_classes = config.candidate_enums.size();
  }

  /**
   * Remove the static methods `valueOf()` and `values()` when safe.
   */
  void remove_enum_generated_methods() {
    optimize_enums::EnumAnalyzeGeneratedMethods analyzer;

    ConcurrentSet<const DexType*> types_used_as_instance_fields;
    walk::parallel::classes(m_scope, [&](DexClass* cls) {
      // We conservatively reject all enums that are instance fields of classes
      // because we don't know if the classes will be serialized or not.
      for (auto& ifield : cls->get_ifields()) {
        types_used_as_instance_fields.insert(
            type::get_element_type_if_array(ifield->get_type()));
      }
    });

    auto should_consider_enum = [&](DexClass* cls) {
      // Only consider enums that are final, not external, do not have
      // interfaces, and are not instance fields of any classes.
      return is_enum(cls) && !cls->is_external() && is_final(cls) &&
             can_delete(cls) && cls->get_interfaces()->size() == 0 &&
             !types_used_as_instance_fields.count(cls->get_type());
    };

    walk::parallel::classes(
        m_scope, [&should_consider_enum, &analyzer](DexClass* cls) {
          if (should_consider_enum(cls)) {
            auto& dmethods = cls->get_dmethods();
            auto valueof_mit = std::find_if(dmethods.begin(), dmethods.end(),
                                            optimize_enums::is_enum_valueof);
            auto values_mit = std::find_if(dmethods.begin(), dmethods.end(),
                                           optimize_enums::is_enum_values);
            if (valueof_mit != dmethods.end() && values_mit != dmethods.end()) {
              analyzer.consider_enum_type(cls->get_type(), *valueof_mit,
                                          *values_mit);
            }
          }
        });

    m_stats.num_candidate_generated_methods =
        analyzer.num_candidate_enum_methods();
    m_stats.num_removed_generated_methods = analyzer.transform_code(m_scope);
  }

 private:
  /**
   * There is usually one synthetic static field in enum class, typically named
   * "$VALUES", but also may be renamed.
   * Return true if there is only one static synthetic field in the class,
   * otherwise return false.
   */
  bool only_one_static_synth_field(const DexClass* cls) {
    DexField* synth_field = nullptr;
    auto synth_access = optimize_enums::synth_access();
    for (auto field : cls->get_sfields()) {
      if (check_required_access_flags(synth_access, field->get_access())) {
        if (synth_field) {
          TRACE(ENUM, 2, "Multiple synthetic fields %s %s", SHOW(synth_field),
                SHOW(field));
          return false;
        }
        synth_field = field;
      }
    }
    if (!synth_field) {
      TRACE(ENUM, 2, "No synthetic field found on %s", SHOW(cls));
      return false;
    }
    return true;
  }

  /**
   * Returns true if the constructor invokes `Enum.<init>`, sets its instance
   * fields, and then returns. We want to make sure there are no side affects.
   *
   * SubEnum.<init>:(Ljava/lang/String;I[other parameters...])V
   * load-param * // multiple load parameter instructions
   * invoke-direct {} Ljava/lang/Enum;.<init>:(Ljava/lang/String;I)V
   * (iput|const) * // put/const instructions for primitive instance fields
   * return-void
   */
  static bool is_simple_enum_constructor(const DexClass* cls,
                                         const DexMethod* method) {
    const auto* params = method->get_proto()->get_args();
    if (!is_private(method) || params->size() < 2) {
      return false;
    }

    auto code = InstructionIterable(method->get_code());
    auto it = code.begin();
    // Load parameter instructions.
    while (it != code.end() && opcode::is_a_load_param(it->insn->opcode())) {
      ++it;
    }
    if (it == code.end()) {
      return false;
    }

    // invoke-direct {} Ljava/lang/Enum;.<init>:(Ljava/lang/String;I)V
    if (!opcode::is_invoke_direct(it->insn->opcode())) {
      return false;
    } else {
      const DexMethodRef* ref = it->insn->get_method();
      // Enum.<init>
      if (ref->get_class() != type::java_lang_Enum() ||
          !method::is_constructor(ref)) {
        return false;
      }
    }
    if (++it == code.end()) {
      return false;
    }

    auto is_iput_or_const = [](IROpcode opcode) {
      // `const-string` is followed by `move-result-pseudo-object`
      return opcode::is_an_iput(opcode) || opcode::is_a_literal_const(opcode) ||
             opcode == OPCODE_CONST_STRING ||
             opcode == IOPCODE_MOVE_RESULT_PSEUDO_OBJECT;
    };
    while (it != code.end() && is_iput_or_const(it->insn->opcode())) {
      ++it;
    }
    if (it == code.end()) {
      return false;
    }

    // return-void is the last instruction
    return opcode::is_return_void(it->insn->opcode()) && (++it) == code.end();
  }

  /**
   * We determine which classes are generated based on:
   * - classes that only have 1 dmethods: <clinit>
   * - no instance fields, nor virtual methods
   * - all static fields match `$SwitchMap$<enum_path>`
   */
  std::vector<DexClass*> collect_generated_classes() {
    std::vector<DexClass*> generated_classes;

    // To avoid any cross store references, only accept generated classes
    // that are in the root store (same for the Enums they reference).
    XStoreRefs xstores(m_stores);

    for (const auto& cls : m_scope) {
      size_t cls_store_idx = xstores.get_store_idx(cls->get_type());
      if (cls_store_idx > 1) {
        continue;
      }

      auto& sfields = cls->get_sfields();
      const auto all_sfield_names_contain = [&sfields](const char* sub) {
        return std::all_of(
            sfields.begin(), sfields.end(), [sub](DexField* sfield) {
              const auto& deobfuscated_name = sfield->get_deobfuscated_name();
              const auto& name = deobfuscated_name.empty()
                                     ? sfield->get_name()->str()
                                     : deobfuscated_name;
              return name.find(sub) != std::string::npos;
            });
      };

      // We expect the generated classes to ONLY contain the lookup tables
      // and the static initializer (<clinit>)
      //
      // Lookup tables for Java Enums all contain $SwitchMap$ in the field name
      // and lookup tables for Kotlin Enums all contain $EnumSwitchMapping$ in
      // the field name.  The two are not expected to mix in a single generated
      // class.
      if (!sfields.empty() && cls->get_dmethods().size() == 1 &&
          cls->get_vmethods().empty() && cls->get_ifields().empty()) {
        if (all_sfield_names_contain("$SwitchMap$") ||
            all_sfield_names_contain("$EnumSwitchMapping$")) {
          generated_classes.emplace_back(cls);
        }
      }
    }

    // Update stats.
    m_stats.num_synthetic_classes = generated_classes.size();

    return generated_classes;
  }

  EnumFieldToOrdinal collect_enum_field_ordinals() {
    EnumFieldToOrdinal enum_field_to_ordinal;

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
  void collect_enum_field_ordinals(const DexClass* cls,
                                   EnumFieldToOrdinal& enum_field_to_ordinal) {
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
      const LookupTableToEnum& lookup_table_to_enum,
      const EnumFieldToOrdinal& enum_field_to_ordinal,
      const GeneratedSwitchCases& generated_switch_cases) {

    namespace cp = constant_propagation;
    walk::parallel::code(m_scope, [&](DexMethod*, IRCode& code) {
      cfg::ScopedCFG cfg(&code);
      cfg->calculate_exit_block();

      optimize_enums::Iterator fixpoint(cfg.get());
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
                                      generated_switch_cases, info)) {
          continue;
        }
        remove_lookup_table_usage(enum_field_to_ordinal, generated_switch_cases,
                                  info);
      }
    });
  }

  /**
   * Check to make sure this is a valid match. Return false to abort the
   * optimization.
   */
  bool check_lookup_table_usage(
      const LookupTableToEnum& lookup_table_to_enum,
      const GeneratedSwitchCases& generated_switch_cases,
      const optimize_enums::Info& info) {

    // Check this is called on an enum.
    auto invoke_ordinal = (*info.invoke)->insn;
    auto invoke_type = invoke_ordinal->get_method()->get_class();
    auto invoke_cls = type_class(invoke_type);
    if (!invoke_cls ||
        (invoke_type != type::java_lang_Enum() && !is_enum(invoke_cls))) {
      return false;
    }

    auto lookup_table = info.array_field;
    if (!lookup_table || lookup_table_to_enum.count(lookup_table) == 0) {
      return false;
    }

    // Check the current enum corresponds.
    auto current_enum = lookup_table_to_enum.at(lookup_table);
    if (invoke_type != type::java_lang_Enum() && current_enum != invoke_type) {
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
      const EnumFieldToOrdinal& enum_field_to_ordinal,
      const GeneratedSwitchCases& generated_switch_cases,
      const optimize_enums::Info& info) {
    auto& cfg = info.branch->cfg();
    auto branch_block = info.branch->block();

    // Use the SwitchEquivFinder to handle not just switch statements but also
    // trees of if and switch statements
    SwitchEquivFinder finder(&cfg, *info.branch, *info.reg,
                             50 /* leaf_duplication_threshold */);
    if (!finder.success()) {
      ++m_stats.num_switch_equiv_finder_failures;
      return;
    }

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
            TRACE(ENUM, 4, "adding %s to B%zu", SHOW(copy), leaf->id());
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
      if (search != field_enum_map.end()) {
        auto field_enum = search->second;
        auto new_case_key = enum_field_to_ordinal.at(field_enum);
        cases.emplace_back(new_case_key, leaf);
      } else {
        // Ignore blocks with negative case key, which should be dead code.
        always_assert_log(
            *old_case_key < 0,
            "can't find case key %d leaving block %zu\n%s\nin %s\n",
            *old_case_key, branch_block->id(), info.str().c_str(), SHOW(cfg));
      }
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
    if (move_ordinal_it.is_end()) {
      return;
    }

    // Remove the switch statement so we can rebuild it with the correct case
    // keys. This removes all edges to the if-else blocks and the blocks will
    // eventually be removed by cfg.simplify()
    cfg.remove_insn(*info.branch);

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
      // Dex Lowering will decide if packed or sparse would be better
      IRInstruction* new_switch = new IRInstruction(OPCODE_SWITCH);
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
   * In the following example, `lookup_table` corresponds to `$SwitchMap$Foo`,
   * and `clinit_cfg` is expected to be the body of the static initializer:
   *
   *   private static class $1 {
   *     public static final synthetic int[] $SwitchMap$Foo;
   *     static {
   *       $SwitchMap$Foo = new int[Foo.values().length];
   *       $SwitchMap$Foo[Foo.Bar.ordinal()] = 1;
   *       $SwitchMap$Foo[Foo.Baz.ordinal()] = 2;
   *       // ...
   *     }
   *   }
   *
   * This function finds the enum class corresponding to `lookup_table` (`Foo`
   * in the example) by tracing back from its initialization:
   *
   *   invoke-static             {}, LFoo;.values:()[LFoo;   <- Find this,
   *   move-result-object        v0
   *   array-length              v0
   *   move-result-pseudo        v1
   *   new-array                 v1
   *   move-result-pseudo-object v2
   *   sput-object               v2, $1;.$SwitchMap$Foo:[I   <- Starting here.
   *
   * Populates `mapping` with all the Enum types corresponding to lookup table
   * fields initialised in the provided CFG.
   */
  void associate_lookup_tables_to_enums(DexClass* generated_cls,
                                        cfg::ControlFlowGraph& clinit_cfg,
                                        const EnumTypes& collected_enums,
                                        LookupTableToEnum& mapping) {
    mf::flow_t f;

    auto m__invoke_values = m::invoke_static_(m::has_method(
        m::named<DexMethodRef>("values") &&
        m::member_of<DexMethodRef>(m::in<DexType*>(collected_enums))));
    auto m__sput_lookup = m::sput_object_(m::has_field(
        m::member_of<DexFieldRef>(m::equals(generated_cls->get_type()))));

    auto uniq = mf::alias | mf::unique;
    auto vals = f.insn(m__invoke_values);
    auto alen = f.insn(m::array_length_()).src(0, vals, uniq);
    auto newa = f.insn(m::new_array_()).src(0, alen, uniq);
    auto sput = f.insn(m__sput_lookup).src(0, newa, uniq);

    auto res = f.find(clinit_cfg, sput);
    for (auto* insn_sput : res.matching(sput)) {
      auto* insn_newa = res.matching(sput, insn_sput, 0).unique();
      auto* insn_alen = res.matching(newa, insn_newa, 0).unique();
      auto* insn_vals = res.matching(alen, insn_alen, 0).unique();
      always_assert(insn_vals && "sput only valid if unique vals exists.");

      auto* lookup_field =
          resolve_field(insn_sput->get_field(), FieldSearch::Static);
      auto* enum_type = insn_vals->get_method()->get_class();

      always_assert(lookup_field);
      mapping.emplace(lookup_field, enum_type);
    }
  }

  Scope m_scope;
  DexStoresVector& m_stores;

  struct Stats {
    size_t num_synthetic_classes{0};
    size_t num_lookup_tables{0};
    size_t num_enum_classes{0};
    size_t num_enum_objs{0};
    size_t num_int_objs{0};
    std::atomic<size_t> num_switch_equiv_finder_failures{0};
    size_t num_candidate_generated_methods{0};
    size_t num_removed_generated_methods{0};
  };
  Stats m_stats;

  ConcurrentSet<DexField*> m_lookup_tables_replaced;
  const DexMethod* m_java_enum_ctor;
  const ProguardMap& m_pg_map;
};

} // namespace

namespace optimize_enums {

void OptimizeEnumsPass::bind_config() {
  bind("max_enum_size", 100, m_max_enum_size,
       "The maximum number of enum field substitutions that are generated and "
       "stored in primary dex.");
  bind("break_reference_equality_allowlist", {}, m_enum_to_integer_allowlist,
       "A allowlist of enum classes that may have more than `max_enum_size` "
       "enum fields, try to erase them without considering reference equality "
       "of the enum objects. Do not add enums to the allowlist!");
}

void OptimizeEnumsPass::run_pass(DexStoresVector& stores,
                                 ConfigFiles& conf,
                                 PassManager& mgr) {
  OptimizeEnums opt_enums(stores, conf);
  opt_enums.remove_redundant_generated_classes();
  opt_enums.replace_enum_with_int(m_max_enum_size, m_enum_to_integer_allowlist);
  opt_enums.remove_enum_generated_methods();
  opt_enums.stats(mgr);
}

static OptimizeEnumsPass s_pass;

} // namespace optimize_enums
