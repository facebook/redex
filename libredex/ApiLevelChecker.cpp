/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ApiLevelChecker.h"

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace api {

// These initial values are bogus until `init()` is called. We can't initialize
// these values until g_redex exists and the classes have been loaded from the
// dex file
int32_t LevelChecker::s_min_level = 0;
DexType* LevelChecker::s_requires_api_old = nullptr;
DexType* LevelChecker::s_requires_api_new = nullptr;
DexType* LevelChecker::s_target_api = nullptr;
bool LevelChecker::s_has_been_init = false;

void LevelChecker::init(int32_t min_level, const Scope& scope) {
  always_assert(min_level >= 0);

  s_has_been_init = true;
  s_min_level = min_level;
  s_requires_api_old =
      DexType::get_type("Landroid/support/annotation/RequiresApi;");
  s_requires_api_new = DexType::get_type("Landroidx/annotation/RequiresApi;");
  s_target_api = DexType::get_type("Landroid/annotation/TargetApi;");
  if (s_requires_api_old == nullptr && s_requires_api_new == nullptr) {
    fprintf(stderr,
            "WARNING: Unable to find RequiresApi annotation. It's either "
            "unused (okay) or been deleted (not okay)\n");
  }
  if (s_target_api == nullptr) {
    fprintf(stderr,
            "WARNING: Unable to find TargetApi annotation. It's either "
            "unused (okay) or been deleted (not okay)\n");
  }

  walk::parallel::classes(scope, init_class);
  walk::parallel::methods(scope, init_method);
  propagate_levels(scope);
}

int32_t LevelChecker::get_method_level(const DexMethod* method) {
  always_assert_log(s_has_been_init, "must call init first");
  int32_t method_level = method->rstate.get_api_level();
  if (method_level == -1) {
    // We need to initialize the API level. Note that there might be a race,
    // and multiple threads might be initializing the same methods (and class).
    // However, they will arrive at the same conclusion, and no shared data
    // structures are mutated along the way that are not thread-safe.

    // must have been created later on by Redex
    DexClass* cls = type_class(method->get_class());
    int32_t class_level = cls->rstate.get_api_level();
    if (class_level == -1) {
      // must have been created later on by Redex
      init_class(cls);
    }

    init_method(const_cast<DexMethod*>(method));
    method_level = method->rstate.get_api_level();
  }

  return method_level;
}

void LevelChecker::init_class(DexClass* clazz) {
  for (DexClass* cls = clazz; cls != nullptr; cls = get_outer_class(cls)) {
    int32_t class_level = get_level(cls);
    if (class_level != -1) {
      clazz->rstate.set_api_level(class_level);
      return;
    }
  }

  clazz->rstate.set_api_level(s_min_level);
}

void LevelChecker::init_method(DexMethod* method) {
  int32_t method_level = get_level(method);
  if (method_level == -1) {
    DexClass* cls = type_class(method->get_class());
    if (cls == nullptr) {
      method_level = s_min_level;
    } else {
      method_level = cls->rstate.get_api_level();
      always_assert(method_level != -1);
    }
  }

  method->rstate.set_api_level(method_level);
}

DexClass* LevelChecker::get_outer_class(const DexClass* cls) {
  TraceContext context(cls->get_type());
  const auto cls_name = cls->get_deobfuscated_name_or_empty();
  auto cash_idx = cls_name.find_last_of('$');
  if (cash_idx == std::string::npos) {
    // this is not an inner class
    return nullptr;
  }
  auto slash_idx = cls_name.find_last_of('/');
  if (slash_idx == std::string::npos || slash_idx < cash_idx) {
    // there's a $ in the class name
    const std::string& outer_name = cls_name.substr(0, cash_idx) + ';';
    DexType* outer = DexType::get_type(outer_name);
    if (outer == nullptr) {
      TRACE(MMINL, 4, "Can't find outer class! %s -> %s", cls_name.data(),
            outer_name.c_str());
      return nullptr;
    }
    DexClass* outer_cls = type_class(outer);
    if (cls == nullptr) {
      TRACE(MMINL, 4, "outer class %s is external?", SHOW(outer));
      return nullptr;
    }
    return outer_cls;
  }
  return nullptr;
}

int32_t LevelChecker::get_min_level() {
  always_assert_log(s_has_been_init, "must call init first");
  return s_min_level;
}

constexpr const char* ANDROID_SDK_PREFIX = "Landroid/";
constexpr const char* ANDROID_X_PREFIX = "Landroidx/";
constexpr const char* ANDROID_SUPPORT_LIB_PREFIX = "Landroid/support/";

bool is_android_sdk_type(const DexType* type) {
  const auto name = type->str();
  return boost::starts_with(name, ANDROID_SDK_PREFIX);
}

bool is_support_lib_type(const DexType* type) {
  const auto name = type->str();
  return boost::starts_with(name, ANDROID_X_PREFIX) ||
         boost::starts_with(name, ANDROID_SUPPORT_LIB_PREFIX);
}

namespace {

/**
 * Assumes min api was setup for all classes and propagates those down the
 * hierarchy.
 */
void propagate_levels(const ClassHierarchy& ch,
                      DexClass* cls,
                      int32_t min_level) {
  int32_t current_min_level = cls->rstate.get_api_level();
  min_level = std::max(min_level, current_min_level);

  auto* intfs = cls->get_interfaces();
  if (intfs != nullptr) {
    for (auto* intf : *intfs) {
      auto* intf_cls = type_class(intf);
      if (intf_cls != nullptr) {
        min_level =
            std::max(min_level, (int32_t)intf_cls->rstate.get_api_level());
      }
    }
  }

  if (current_min_level < min_level) {
    cls->rstate.set_api_level(min_level);
  }

  const auto& children_types = get_children(ch, cls->get_type());
  for (const DexType* children_type : children_types) {
    auto* children_cls = type_class(children_type);
    propagate_levels(ch, children_cls, min_level);
  }
}

} // namespace

void LevelChecker::propagate_levels(const Scope& scope) {
  const auto* obj_type = type::java_lang_Object();
  ClassHierarchy ch = build_type_hierarchy(scope);

  // First propagate levels for interfaces.
  for (DexClass* cls : scope) {
    if (is_interface(cls) && cls->get_super_class() == obj_type) {
      ::api::propagate_levels(ch, cls, s_min_level);
    }
  }

  // And then for the rest.
  for (DexClass* cls : scope) {
    if (!is_interface(cls) && cls->get_super_class() == obj_type) {
      ::api::propagate_levels(ch, cls, s_min_level);
    }
  }
}

} // namespace api
