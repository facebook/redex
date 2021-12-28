/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PrintKotlinStats.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "KotlinNullCheckMethods.h"
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

// Check if cls is from Kotlin source
bool is_kotlin_class(DexClass* cls) {
  auto src_string = cls->get_source_file();
  if (src_string && boost::algorithm::ends_with(src_string->str(), ".kt")) {
    return true;
  }
  return false;
}

// Check cls name is in anonymous format
// Anonymous cls name ends with \$[0-9]*
bool is_anonymous(std::string name) {
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
                                ConfigFiles&,
                                PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  std::unordered_set<DexType*> delegate_types{
      DexType::get_type(KPROPERTY_ARRAY),
      DexType::get_type(R_PROP_SIGNATURE),
      DexType::get_type(W_PROP_SIGNATURE),
      DexType::get_type(RW_PROP_SIGNATURE),
  };
  std::unordered_set<DexType*> lazy_delegate_types{
      DexType::get_type(LAZY_SIGNATURE),
  };

  // Handle methods
  m_stats = walk::parallel::methods<Stats>(
      scope, [&](DexMethod* method) { return handle_method(method); });

  // Handle fields
  // Count delegated properties
  walk::fields(scope, [&](DexField* field) {
    auto typ = field->get_type();
    if (lazy_delegate_types.count(typ)) {
      m_stats.kotlin_lazy_delegates++;
    }
    if (delegate_types.count(typ)) {
      m_stats.kotlin_delegates++;
    }
  });

  // Handle classes
  std::mutex mtx;
  walk::parallel::classes(scope, [&](DexClass* cls) {
    auto local_stats = handle_class(cls);
    std::lock_guard g(mtx);
    m_stats += local_stats;
  });
  m_stats.report(mgr);
}

PrintKotlinStats::Stats PrintKotlinStats::handle_class(DexClass* cls) {
  Stats stats;
  bool is_lambda = false;
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
      }
      stats.kotlin_class_with_instance++;
    }
  }
  if (cls->rstate.is_cls_kotlin()) {
    stats.kotlin_class++;
    for (auto* method : cls->get_all_methods()) {
      if (boost::algorithm::ends_with(method->get_name()->str(), "$default")) {
        stats.kotlin_default_arg_method++;
      }
    }
    if (is_anonymous(cls->get_name()->str())) {
      stats.kotlin_anonymous_class++;
    }
    if (boost::algorithm::ends_with(cls->get_name()->str(), "$Companion;")) {
      stats.kotlin_companion_class++;
    }
  }
  return stats;
}

PrintKotlinStats::Stats PrintKotlinStats::handle_method(DexMethod* method) {
  Stats stats;

  if (!method->get_code()) {
    return stats;
  }

  DexClass* cls = type_class(method->get_class());
  if (!cls) {
    return stats;
  }

  if (method->get_access() & ACC_PUBLIC) {
    auto* arg_types = method->get_proto()->get_args();
    for (auto arg_type : *arg_types) {
      if (cls->rstate.is_cls_kotlin()) {
        stats.kotlin_public_param_objects++;
      } else {
        stats.java_public_param_objects++;
      }
    }
  }

  auto code = method->get_code();

  for (const auto& it : InstructionIterable(code)) {
    auto insn = it.insn;
    switch (insn->opcode()) {
    case OPCODE_INVOKE_STATIC: {
      auto called_method = insn->get_method();
      if (m_kotlin_null_assertions.count(called_method)) {
        stats.kotlin_null_check_insns++;
      }
    } break;
    default:
      break;
    }
  }
  return stats;
}

void PrintKotlinStats::Stats::report(PassManager& mgr) const {
  mgr.incr_metric("kotlin_null_check_insns", kotlin_null_check_insns);
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
  mgr.incr_metric("kotlin_coroutine_continuation_base",
                  kotlin_coroutine_continuation_base);

  TRACE(KOTLIN_STATS, 1, "KOTLIN_STATS: kotlin_null_check_insns = %zu",
        kotlin_null_check_insns);
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
        "KOTLIN_STATS: kotlin_coroutine_continuation_base = %zu",
        kotlin_coroutine_continuation_base);
}

static PrintKotlinStats s_pass;
