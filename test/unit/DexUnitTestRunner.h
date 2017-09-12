/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <boost/any.hpp>
#include <mutex>

#include "Creators.h"
#include "Debug.h"
#include "DexClass.h"
#include "PassManager.h"
#include "RedexContext.h"

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

  DexClass* create_class(std::string name) {
    auto type = DexType::make_type(DexString::make_string(name));
    ClassCreator creator(type);
    creator.set_super(get_object_type());
    auto cls = creator.create();
    auto clinit_name = DexString::make_string("<clinit>");
    auto void_args = DexTypeList::make_type_list({});
    auto void_void = DexProto::make_proto(get_void_type(), void_args);
    auto clinit = static_cast<DexMethod*>(
        DexMethod::make_method(type, clinit_name, void_void));
    clinit->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_CONSTRUCTOR, false);
    clinit->set_code(std::make_unique<IRCode>(clinit, 1));
    cls->add_method(clinit);
    m_stores.back().add_classes({cls});
    return cls;
  }

  DexEncodedValue* make_ev(DexType* type, boost::any val) {
    if (val.type() == typeid(uint64_t)) {
      auto ev = DexEncodedValue::zero_for_type(type);
      ev->value(boost::any_cast<uint64_t>(val));
      return ev;
    } else {
      always_assert(val.type() == typeid(DexString*));
      return new DexEncodedValueString(boost::any_cast<DexString*>(val));
    }
  }

  DexField* add_concrete_field(DexClass* cls,
                               const std::string& name,
                               DexType* type,
                               boost::any val) {
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
    Scope external_classes;
    Json::Value conf_obj = Json::nullValue;
    ConfigFiles dummy_cfg(conf_obj);
    manager.run_passes(m_stores, external_classes, dummy_cfg);
  }

 private:
  std::vector<DexStore> m_stores;
};

std::mutex DexUnitTestRunner::g_setup_lock;
