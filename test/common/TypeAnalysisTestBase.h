/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <sparta/PatriciaTreeSet.h>

#include "DexTypeEnvironment.h"
#include "RedexTest.h"

using TypeSet = sparta::PatriciaTreeSet<const DexType*>;

class TypeAnalysisTestBase : public RedexIntegrationTest {
 protected:
  void set_root_method(const std::string& full_name) {
    auto method = DexMethod::get_method(full_name)->as_def();
    method->rstate.set_root();
  }

  DexMethod* get_method(const std::string& name) {
    std::string full_name = "Lcom/facebook/redextest/" + name;
    return DexMethod::get_method(full_name)->as_def();
  }

  DexMethod* get_method(const std::string& name, const std::string& rtype) {
    std::string full_name = "Lcom/facebook/redextest/" + name +
                            ":()Lcom/facebook/redextest/" + rtype + ";";
    return DexMethod::get_method(full_name)->as_def();
  }

  DexMethod* get_method(const std::string& name,
                        const std::string& params,
                        const std::string& rtype) {
    std::string full_name =
        "Lcom/facebook/redextest/" + name + ":(" + params + ")" + rtype;
    return DexMethod::get_method(full_name)->as_def();
  }

  DexField* get_field(const std::string& name) {
    std::string full_name = "Lcom/facebook/redextest/" + name;
    return DexField::get_field(full_name)->as_def();
  }

  DexTypeDomain get_type_domain(const std::string& type_name) {
    std::string full_name = "Lcom/facebook/redextest/" + type_name + ";";
    return DexTypeDomain::create_not_null(
        DexType::make_type(DexString::make_string(full_name)));
  }

  DexTypeDomain get_type_domain_simple(const std::string& type_name,
                                       bool is_not_null = false) {
    if (is_not_null) {
      return DexTypeDomain::create_not_null(
          DexType::make_type(DexString::make_string(type_name)));
    }

    return DexTypeDomain::create_nullable(
        DexType::make_type(DexString::make_string(type_name)));
  }

  DexType* get_type_simple(const std::string& type_name) {
    return DexType::make_type(DexString::make_string(type_name));
  }

  DexType* get_type(const std::string& type_name) {
    std::string full_name = "Lcom/facebook/redextest/" + type_name + ";";
    return DexType::make_type(DexString::make_string(full_name));
  }

  TypeSet get_type_set(std::initializer_list<DexType*> l) {
    TypeSet s;
    for (const auto elem : l) {
      s.insert(const_cast<const DexType*>(elem));
    }
    return s;
  }

  SmallSetDexTypeDomain get_small_set_domain(
      std::initializer_list<const std::string> l) {
    SmallSetDexTypeDomain s;
    for (const auto& elem : l) {
      auto type = get_type(elem);
      auto domain = SmallSetDexTypeDomain(type);
      s.join_with(domain);
    }
    return s;
  }
};
