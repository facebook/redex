/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RefChecker.h"
#include "TypeUtil.h"

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
        // Object in it, but in some tests we don't set up the full environment
        // and do need this.
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
    for (auto t : interfaces->get_type_list()) {
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
  for (auto t : args->get_type_list()) {
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
