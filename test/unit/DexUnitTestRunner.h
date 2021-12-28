/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/variant.hpp>
#include <json/value.h>
#include <mutex>
#include <utility>

#include "ConfigFiles.h"
#include "Creators.h"
#include "Debug.h"
#include "DexClass.h"
#include "PassManager.h"
#include "RedexContext.h"

using EvType = boost::variant<uint64_t, const DexString*>;

class DexUnitTestRunner {
  static std::mutex g_setup_lock;

 public:
  DexUnitTestRunner() {
    std::lock_guard<std::mutex> guard(g_setup_lock);
    if (g_redex == nullptr) {
      g_redex = new RedexContext();
    }
    DexMetadata dm;
    dm.set_id("test_store");
    m_stores.emplace_back(dm);
  }

  DexClass* create_class(const std::string& name) {
    auto type = DexType::make_type(DexString::make_string(name));
    ClassCreator creator(type);
    creator.set_super(type::java_lang_Object());
    auto cls = creator.create();
    auto clinit_name = DexString::make_string("<clinit>");
    auto void_args = DexTypeList::make_type_list({});
    auto void_void = DexProto::make_proto(type::_void(), void_args);
    auto clinit = static_cast<DexMethod*>(
        DexMethod::make_method(type, clinit_name, void_void));
    clinit->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_CONSTRUCTOR, false);
    clinit->set_code(std::make_unique<IRCode>(clinit, 1));
    cls->add_method(clinit);
    m_stores.back().add_classes({cls});
    return cls;
  }

  static DexEncodedValue* make_ev(DexType* type, const EvType& val) {
    if (val.which() == 0) {
      auto ev = DexEncodedValue::zero_for_type(type);
      ev->value(boost::get<uint64_t>(val));
      return ev;
    } else {
      return new DexEncodedValueString(boost::get<const DexString*>(val));
    }
  }

  static DexField* add_concrete_field(DexClass* cls,
                                      const std::string& name,
                                      DexType* type,
                                      const EvType& val) {
    auto container = cls->get_type();
    auto field_name = DexString::make_string(name);
    auto field = static_cast<DexField*>(
        DexField::make_field(container, field_name, type));
    auto ev = make_ev(type, val);
    field->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL, ev);
    cls->add_field(field);
    return field;
  }

  void run(Pass* pass) {
    std::vector<Pass*> passes = {pass};
    PassManager manager(passes);
    manager.set_testing_mode();
    Json::Value conf_obj = Json::nullValue;
    ConfigFiles dummy_cfg(conf_obj);
    manager.run_passes(m_stores, dummy_cfg);
  }

 private:
  std::vector<DexStore> m_stores;
};

std::mutex DexUnitTestRunner::g_setup_lock;
