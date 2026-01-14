/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrintKotlinStats.h"
#include "ConfigFiles.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "KotlinNullCheckMethods.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "Show.h"
#include "Walkers.h"

namespace {
constexpr const char* LAZY_SIGNATURE = "Lkotlin/Lazy;";
constexpr const char* R_PROP_SIGNATURE = "Lkotlin/properties/ReadProperty;";
constexpr const char* W_PROP_SIGNATURE = "Lkotlin/properties/WriteProperty;";
constexpr const char* RW_PROP_SIGNATURE =
    "Lkotlin/properties/ReadWriteProperty;";
constexpr const char* KPROPERTY_ARRAY = "[Lkotlin/reflect/KProperty;";
constexpr const char* KOTLIN_LAMBDA = "Lkotlin/jvm/internal/Lambda;";
constexpr const char* DI_BASE = "Lcom/facebook/inject/AbstractLibraryModule;";
constexpr const char* CONTINUATION_IMPL =
    "Lkotlin/coroutines/jvm/internal/ContinuationImpl;";
// A lambda like { true } has 4 instructions.
constexpr size_t TRIVIAL_LAMBDA_NUM_INSTRUCTIONS_THRESHOLD = 4u;

// Serialize cfg code to a string.
std::string serialize_cfg_code(const cfg::ControlFlowGraph& cfg) {
  std::ostringstream ss;
  for (const auto& mie : cfg::ConstInstructionIterable(cfg)) {
    ss << show(mie.insn) << '\n';
  }
  return ss.str();
}

// Check if cls is from Kotlin source
bool is_kotlin_class(DexClass* cls) {
  const auto* src_string = cls->get_source_file();
  return (src_string != nullptr) &&
         boost::algorithm::ends_with(src_string->str(), ".kt");
}

// Check cls name is in anonymous format
// Anonymous cls name ends with \$[0-9]*
bool is_anonymous(std::string_view name) {
  auto last = name.find_last_of('$');
  if (last == std::string::npos) {
    return false;
  }
  // Except for the ; at the end
  for (size_t i = last + 1; i < name.length() - 1; ++i) {
    if (name[i] < '0' || name[i] > '9') {
      return false;
    }
  }
  return true;
}

bool is_kotlin_default_arg_method(const DexMethod& method) {
  const auto* args = method.get_proto()->get_args();
  // The last two args are the defaults bitset and an unused Object.
  return method.get_name()->str().ends_with("$default") && args->size() > 2 &&
         args->at(args->size() - 1) ==
             DexType::get_type("Ljava/lang/Object;") &&
         args->at(args->size() - 2) == DexType::get_type("I");
}

bool is_composable_method(const DexMethod* method) {
  const auto* anno_set = method->get_anno_set();
  if (anno_set == nullptr) {
    return false;
  }
  std::vector<const DexType*> types;
  anno_set->gather_types(types);
  return std::find(types.begin(), types.end(),
                   DexType::get_type(
                       "Landroidx/compose/runtime/Composable;")) != types.end();
}

// Check if a method is hot based on call_count threshold.
// Returns false if method_profiles is null or has no stats.
bool is_method_hot_by_call_count(
    const DexMethod* method,
    const method_profiles::MethodProfiles* method_profiles,
    float call_count_threshold) {
  if (method_profiles == nullptr || !method_profiles->has_stats()) {
    return false;
  }
  for (const auto& [interaction_id, stats_map] :
       method_profiles->all_interactions()) {
    auto it = stats_map.find(method);
    if (it != stats_map.end() && it->second.call_count > call_count_threshold) {
      return true;
    }
  }
  return false;
}

constexpr float kHotDefaultArgCallCountThreshold = 5.0f;

// Wrapper for hot default arg detection using fixed threshold.
bool is_hot_default_arg_by_call_count(
    const DexMethod* method,
    const method_profiles::MethodProfiles* method_profiles) {
  return is_method_hot_by_call_count(method, method_profiles,
                                     kHotDefaultArgCallCountThreshold);
}

} // namespace

// Setup types/strings needed for the pass
void PrintKotlinStats::setup() {
  m_kotlin_null_assertions =
      kotlin_nullcheck_wrapper::get_kotlin_null_assertions();
  m_kotlin_lambdas_base = DexType::get_type(KOTLIN_LAMBDA);
  m_kotlin_coroutin_continuation_base = DexType::get_type(CONTINUATION_IMPL);
  m_di_base = DexType::get_type(DI_BASE);
  m_instance = DexString::make_string("INSTANCE");
}

// Annotate Kotlin classes before StripDebugInfoPass removes it
void PrintKotlinStats::eval_pass(DexStoresVector& stores,
                                 ConfigFiles&,
                                 PassManager&) {
  Scope scope = build_class_scope(stores);
  setup();
  walk::parallel::classes(scope, [&](DexClass* cls) {
    if (is_kotlin_class(cls)) {
      cls->rstate.set_cls_kotlin();
    }
  });
}

void PrintKotlinStats::run_pass(DexStoresVector& stores,
                                ConfigFiles& conf,
                                PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  UnorderedSet<DexType*> delegate_types{
      DexType::get_type(KPROPERTY_ARRAY),
      DexType::get_type(R_PROP_SIGNATURE),
      DexType::get_type(W_PROP_SIGNATURE),
      DexType::get_type(RW_PROP_SIGNATURE),
  };
  UnorderedSet<DexType*> lazy_delegate_types{
      DexType::get_type(LAZY_SIGNATURE),
  };

  // Handle methods
  m_stats = walk::parallel::methods<Stats>(
      scope, [&](DexMethod* method) { return handle_method(method); });

  // Handle fields
  // Count delegated properties
  walk::fields(scope, [&](DexField* field) {
    auto* typ = field->get_type();
    if (lazy_delegate_types.count(typ) != 0u) {
      m_stats.kotlin_lazy_delegates++;
    }
    if (delegate_types.count(typ) != 0u) {
      m_stats.kotlin_delegates++;
    }
  });

  // Handle classes
  auto& method_profiles = conf.get_method_profiles();
  const method_profiles::MethodProfiles* method_profiles_ptr =
      method_profiles.has_stats() ? &method_profiles : nullptr;
  std::mutex mtx;
  walk::parallel::classes(scope, [&](DexClass* cls) {
    auto local_stats = handle_class(cls, method_profiles_ptr);
    std::lock_guard g(mtx);
    m_stats += local_stats;
  });
  m_stats.report(mgr);
}

PrintKotlinStats::Stats PrintKotlinStats::handle_class(
    DexClass* cls, const method_profiles::MethodProfiles* method_profiles) {
  Stats stats;
  bool is_lambda = false;
  bool is_non_capturing_lambda = false;
  if (cls->get_super_class() == m_kotlin_lambdas_base) {
    stats.kotlin_lambdas++;
    is_lambda = true;
  }
  if (cls->get_super_class() == m_kotlin_coroutin_continuation_base) {
    stats.kotlin_coroutine_continuation_base++;
  }

  if (cls->get_super_class() == m_di_base) {
    stats.di_generated_class++;
  }

  for (auto* field : cls->get_sfields()) {
    if (field->get_name() == m_instance &&
        field->get_type() == cls->get_type()) {
      if (is_lambda) {
        stats.kotlin_non_capturing_lambda++;
        is_non_capturing_lambda = true;
      }
      stats.kotlin_class_with_instance++;
    }
  }
  if (cls->rstate.is_cls_kotlin()) {
    stats.kotlin_class++;
    for (const auto* method : cls->get_all_methods()) {
      if (is_kotlin_default_arg_method(*method)) {
        stats.kotlin_default_arg_method++;
        const auto* args = method->get_proto()->get_args();
        always_assert(args->size() > 2);

        bool is_hot = is_hot_default_arg_by_call_count(method, method_profiles);

        // This also includes arguments that aren't specified as default args
        // at the source code level. We can't reliably have this information.
        switch (args->size() - 2) {
        case 1:
          stats.kotlin_default_arg_1_param++;
          if (is_hot) {
            stats.kotlin_hot_default_arg_1_param++;
          }
          break;
        case 2:
          stats.kotlin_default_arg_2_params++;
          if (is_hot) {
            stats.kotlin_hot_default_arg_2_params++;
          }
          break;
        case 3:
          stats.kotlin_default_arg_3_params++;
          if (is_hot) {
            stats.kotlin_hot_default_arg_3_params++;
          }
          break;
        case 4:
          stats.kotlin_default_arg_4_params++;
          if (is_hot) {
            stats.kotlin_hot_default_arg_4_params++;
          }
          break;
        default:
          stats.kotlin_default_arg_5plus_params++;
          if (is_hot) {
            stats.kotlin_hot_default_arg_5plus_params++;
          }
          break;
        }
      } else if (method->get_name()->str().ends_with("$default")) {
        stats.kotlin_homonym_default_arg_method++;
      }
      if (is_composable_method(method)) {
        stats.kotlin_composable_method++;
      }
      if (is_non_capturing_lambda && method->get_name()->str() == "invoke" &&
          !is_synthetic(method) && method->get_code() != nullptr &&
          method->get_code()->count_opcodes() <=
              TRIVIAL_LAMBDA_NUM_INSTRUCTIONS_THRESHOLD) {
        stats.kotlin_trivial_non_capturing_lambdas++;
        always_assert(method->get_code()->cfg_built());
        bool inserted;
        std::tie(std::ignore, inserted) =
            m_kotlin_unique_trivial_non_capturing_lambdas.insert(
                serialize_cfg_code(method->get_code()->cfg()));
        if (inserted) {
          stats.kotlin_unique_trivial_non_capturing_lambdas++;
        }
      }
    }
    if (is_anonymous(cls->get_name()->str())) {
      stats.kotlin_anonymous_class++;
    }
    if (boost::algorithm::ends_with(cls->get_name()->str(), "$Companion;")) {
      stats.kotlin_companion_class++;
    }
    if (is_enum(cls)) {
      stats.kotlin_enum_class++;
    }
  }
  return stats;
}

PrintKotlinStats::Stats PrintKotlinStats::handle_method(DexMethod* method) {
  Stats stats;

  if (method->get_code() == nullptr) {
    return stats;
  }

  DexClass* cls = type_class(method->get_class());
  if (cls == nullptr) {
    return stats;
  }

  if ((method->get_access() & ACC_PUBLIC) != 0u) {
    auto* arg_types = method->get_proto()->get_args();
    if (cls->rstate.is_cls_kotlin()) {
      stats.kotlin_public_param_objects += arg_types->size();
    } else {
      stats.java_public_param_objects += arg_types->size();
    }
  }

  always_assert(method->get_code()->cfg_built());
  auto& cfg = method->get_code()->cfg();

  for (const auto& it : cfg::InstructionIterable(cfg)) {
    auto* insn = it.insn;
    switch (insn->opcode()) {
    case OPCODE_INVOKE_STATIC: {
      auto* called_method = insn->get_method();
      if (m_kotlin_null_assertions.count(called_method) != 0u) {
        stats.kotlin_null_check_insns++;
      }
    } break;
    case OPCODE_AND_INT_LIT: {
      if (is_kotlin_default_arg_method(*method)) {
        stats.kotlin_default_arg_check_insns++;
      }
      if (is_composable_method(method)) {
        stats.kotlin_composable_and_lit_insns++;
      }
      stats.kotlin_and_lit_insns++;
    } break;
    default:
      break;
    }
  }
  return stats;
}

void PrintKotlinStats::Stats::report(PassManager& mgr) const {
  mgr.incr_metric("kotlin_null_check_insns", kotlin_null_check_insns);
  mgr.incr_metric("kotlin_default_arg_check_insns",
                  kotlin_default_arg_check_insns);
  mgr.incr_metric("kotlin_default_arg_1_param", kotlin_default_arg_1_param);
  mgr.incr_metric("kotlin_default_arg_2_params", kotlin_default_arg_2_params);
  mgr.incr_metric("kotlin_default_arg_3_params", kotlin_default_arg_3_params);
  mgr.incr_metric("kotlin_default_arg_4_params", kotlin_default_arg_4_params);
  mgr.incr_metric("kotlin_default_arg_5plus_params",
                  kotlin_default_arg_5plus_params);
  mgr.incr_metric("kotlin_hot_default_arg_1_param",
                  kotlin_hot_default_arg_1_param);
  mgr.incr_metric("kotlin_hot_default_arg_2_params",
                  kotlin_hot_default_arg_2_params);
  mgr.incr_metric("kotlin_hot_default_arg_3_params",
                  kotlin_hot_default_arg_3_params);
  mgr.incr_metric("kotlin_hot_default_arg_4_params",
                  kotlin_hot_default_arg_4_params);
  mgr.incr_metric("kotlin_hot_default_arg_5plus_params",
                  kotlin_hot_default_arg_5plus_params);
  mgr.incr_metric("kotlin_composable_and_lit_insns",
                  kotlin_composable_and_lit_insns);
  mgr.incr_metric("kotlin_and_lit_insns", kotlin_and_lit_insns);
  mgr.incr_metric("java_public_param_objects", java_public_param_objects);
  mgr.incr_metric("kotlin_public_param_objects", kotlin_public_param_objects);
  mgr.incr_metric("no_of_delegates", kotlin_delegates);
  mgr.incr_metric("no_of_lazy_delegates", kotlin_lazy_delegates);
  mgr.incr_metric("kotlin_lambdas", kotlin_lambdas);
  mgr.incr_metric("kotlin_non_capturing_lambda", kotlin_non_capturing_lambda);
  mgr.incr_metric("kotlin_classes_with_instance", kotlin_class_with_instance);
  mgr.incr_metric("kotlin_class", kotlin_class);
  mgr.incr_metric("Kotlin_anonymous_classes", kotlin_anonymous_class);
  mgr.incr_metric("kotlin_companion_class", kotlin_companion_class);
  mgr.incr_metric("di_generated_class", di_generated_class);
  mgr.incr_metric("kotlin_default_arg_method", kotlin_default_arg_method);
  mgr.incr_metric("kotlin_homonym_default_arg_method",
                  kotlin_homonym_default_arg_method);
  mgr.incr_metric("kotlin_composable_method", kotlin_composable_method);
  mgr.incr_metric("kotlin_coroutine_continuation_base",
                  kotlin_coroutine_continuation_base);
  mgr.incr_metric("kotlin_enum_class", kotlin_enum_class);
  mgr.incr_metric("kotlin_trivial_non_capturing_lambdas",
                  kotlin_trivial_non_capturing_lambdas);
  mgr.incr_metric("kotlin_unique_trivial_non_capturing_lambdas",
                  kotlin_unique_trivial_non_capturing_lambdas);

  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: kotlin_null_check_insns = %zu",
        kotlin_null_check_insns);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: kotlin_hot_default_arg_1_param = %zu",
        kotlin_hot_default_arg_1_param);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: kotlin_hot_default_arg_2_params = %zu",
        kotlin_hot_default_arg_2_params);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: kotlin_hot_default_arg_3_params = %zu",
        kotlin_hot_default_arg_3_params);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: kotlin_hot_default_arg_4_params = %zu",
        kotlin_hot_default_arg_4_params);
  TRACE(KOTLIN_STATS, 1,
        "KOTLIN_STATS: kotlin_hot_default_arg_5plus_params = %zu",
        kotlin_hot_default_arg_5plus_params);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: java_public_param_objects = %zu",
        java_public_param_objects);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: kotlin_public_param_objects = %zu",
        kotlin_public_param_objects);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: no_of_delegates = %zu",
        kotlin_delegates);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: no_of_lazy_delegates = %zu",
        kotlin_lazy_delegates);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: kotlin_lambdas = %zu", kotlin_lambdas);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: kotlin_non_capturing_lambda = %zu",
        kotlin_non_capturing_lambda);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: kotlin_class_with_instance = %zuu",
        kotlin_class_with_instance);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: kotlin_class = %zu", kotlin_class);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: kotlin_anonymous_class = %zu",
        kotlin_anonymous_class);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: kotlin_companion_class = %zu",
        kotlin_companion_class);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: di_generated_class = %zu",
        di_generated_class);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: kotlin_default_arg_method = %zu",
        kotlin_default_arg_method);
  TRACE(KOTLIN_STATS, 1,
        "KOTLIN_STATS: kotlin_homonym_default_arg_method = %zu",
        kotlin_homonym_default_arg_method);
  TRACE(KOTLIN_STATS, 1,
        "KOTLIN_STATS: kotlin_coroutine_continuation_base = %zu",
        kotlin_coroutine_continuation_base);
  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: kotlin_enum_class = %zu",
        kotlin_enum_class);
  TRACE(KOTLIN_STATS, 1,
        "KOTLIN_STATS: kotlin_trivial_non_capturing_lambdas = %zu",
        kotlin_trivial_non_capturing_lambdas);
  TRACE(KOTLIN_STATS, 1,
        "KOTLIN_STATS: kotlin_unique_trivial_non_capturing_lambdas = %zu",
        kotlin_unique_trivial_non_capturing_lambdas);
}

static PrintKotlinStats s_pass;
