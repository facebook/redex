/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RefChecker.h"

#include "EditableCfgAdapter.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"
#include "TypeUtil.h"

CodeRefs::CodeRefs(const DexMethod* method) {
  if (!method->get_code()) {
    return;
  }
  std::unordered_set<const DexType*> types_set;
  std::unordered_set<const DexMethod*> methods_set;
  std::unordered_set<const DexField*> fields_set;
  editable_cfg_adapter::iterate(
      method->get_code(), [&](const MethodItemEntry& mie) {
        auto insn = mie.insn;
        if (insn->has_type()) {
          always_assert(insn->get_type());
          types_set.insert(insn->get_type());
        } else if (insn->has_method()) {
          auto callee_ref = insn->get_method();
          auto callee =
              resolve_method(callee_ref, opcode_to_search(insn), method);
          if (!callee && opcode_to_search(insn) == MethodSearch::Virtual) {
            callee = resolve_method(callee_ref, MethodSearch::InterfaceVirtual,
                                    method);
          }
          if (!callee) {
            invalid_refs = true;
            return editable_cfg_adapter::LOOP_BREAK;
          }
          if (callee != callee_ref) {
            types_set.insert(callee_ref->get_class());
          }
          methods_set.insert(callee);
        } else if (insn->has_field()) {
          auto field_ref = insn->get_field();
          auto field = resolve_field(field_ref);
          if (!field) {
            invalid_refs = true;
            return editable_cfg_adapter::LOOP_BREAK;
          }
          if (field != field_ref) {
            types_set.insert(field_ref->get_class());
          }
          fields_set.insert(field);
        }
        return editable_cfg_adapter::LOOP_CONTINUE;
      });
  if (invalid_refs) {
    return;
  }

  std::vector<DexType*> catch_types;
  method->get_code()->gather_catch_types(catch_types);
  for (auto type : catch_types) {
    if (type) {
      types_set.insert(type);
    }
  }

  types.reserve(types_set.size());
  types.insert(types.end(), types_set.begin(), types_set.end());

  methods.reserve(methods_set.size());
  methods.insert(methods.end(), methods_set.begin(), methods_set.end());

  fields.reserve(fields_set.size());
  fields.insert(fields.end(), fields_set.begin(), fields_set.end());
}

bool RefChecker::check_type(const DexType* type) const {
  auto res = m_type_cache.get(type, boost::none);
  if (res == boost::none) {
    res = check_type_internal(type);
    m_type_cache.update(
        type, [res](const DexType*, boost::optional<bool>& value, bool exists) {
          always_assert(!exists || value == res);
          value = res;
        });
  }
  return *res;
}

bool RefChecker::check_method(const DexMethod* method) const {
  auto res = m_method_cache.get(method, boost::none);
  if (res == boost::none) {
    res = check_method_internal(method);
    m_method_cache.update(
        method,
        [res](const DexMethod*, boost::optional<bool>& value, bool exists) {
          always_assert(!exists || value == res);
          value = res;
        });
  }
  return *res;
}

bool RefChecker::check_field(const DexField* field) const {
  auto res = m_field_cache.get(field, boost::none);
  if (res == boost::none) {
    res = check_field_internal(field);
    m_field_cache.update(
        field,
        [res](const DexField*, boost::optional<bool>& value, bool exists) {
          always_assert(!exists || value == res);
          value = res;
        });
  }
  return *res;
}

bool RefChecker::check_class(
    const DexClass* cls,
    const std::unique_ptr<const method_override_graph::Graph>& mog) const {
  if (!check_type(cls->get_type())) {
    return false;
  }
  const auto fields = cls->get_all_fields();
  if (std::any_of(fields.begin(), fields.end(),
                  [this](DexField* field) { return !check_field(field); })) {
    return false;
  }
  const auto methods = cls->get_all_methods();
  for (const auto* method : methods) {
    if (!check_method_and_code(method)) {
      return false;
    }
    if (mog && method->is_virtual()) {
      if (method_override_graph::any_overridden_methods(
              *mog, method,
              [&](const auto* m) {
                if (!m->is_external()) {
                  return false;
                }
                if (m_min_sdk_api && !m_min_sdk_api->has_method(m)) {
                  TRACE(REFC, 4, "Risky external method override %s -> %s",
                        SHOW(method), SHOW(m));
                  return true;
                }
                return false;
              },
              true)) {
        return false;
      }
    }
  }

  return true;
}

bool RefChecker::check_code_refs(const CodeRefs& code_refs) const {
  if (code_refs.invalid_refs) {
    return false;
  }
  for (auto type : code_refs.types) {
    if (!check_type(type)) {
      return false;
    }
  }
  for (auto method : code_refs.methods) {
    if (!check_method(method)) {
      return false;
    }
  }
  for (auto field : code_refs.fields) {
    if (!check_field(field)) {
      return false;
    }
  }
  return true;
}

bool RefChecker::check_type_internal(const DexType* type) const {
  type = type::get_element_type_if_array(type);
  if (type::is_primitive(type)) {
    return true;
  }
  while (true) {
    auto cls = type_class(type);
    if (cls == nullptr) {
      if (type == type::java_lang_String() || type == type::java_lang_Class() ||
          type == type::java_lang_Enum() || type == type::java_lang_Object() ||
          type == type::java_lang_Void() ||
          type == type::java_lang_Throwable() ||
          type == type::java_lang_Boolean() || type == type::java_lang_Byte() ||
          type == type::java_lang_Short() ||
          type == type::java_lang_Character() ||
          type == type::java_lang_Integer() || type == type::java_lang_Long() ||
          type == type::java_lang_Float() || type == type::java_lang_Double()) {
        // This shouldn't be needed, as ideally we have a min-sdk loaded with
        // Object in it, but in some tests we don't set up the full
        // environment and do need this.
        return true;
      }
      return false;
    }
    if (cls->is_external()) {
      return m_min_sdk_api && m_min_sdk_api->has_type(type);
    }
    if (m_xstores && m_xstores->illegal_ref(m_store_idx, type)) {
      return false;
    }
    auto interfaces = cls->get_interfaces();
    for (auto t : *interfaces) {
      if (!check_type(t)) {
        return false;
      }
    }
    type = cls->get_super_class();
  }
}

bool RefChecker::check_method_internal(const DexMethod* method) const {
  auto cls = type_class(method->get_class());
  if (cls->is_external()) {
    return m_min_sdk_api && m_min_sdk_api->has_method(method);
  }
  if (!check_type(method->get_class())) {
    return false;
  }
  auto args = method->get_proto()->get_args();
  for (auto t : *args) {
    if (!check_type(t)) {
      return false;
    }
  }
  return check_type(method->get_proto()->get_rtype());
}

bool RefChecker::check_field_internal(const DexField* field) const {
  auto cls = type_class(field->get_class());
  if (cls->is_external()) {
    return m_min_sdk_api && m_min_sdk_api->has_field(field);
  }
  return check_type(field->get_class()) && check_type(field->get_type());
}

bool RefChecker::is_in_primary_dex(const DexType* type) const {
  return m_xstores && m_xstores->is_in_primary_dex(type);
}
