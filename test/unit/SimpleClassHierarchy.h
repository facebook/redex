/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>

#include "Creators.h"
#include "DexClass.h"
#include "TypeUtil.h"

namespace redex {
namespace test {

struct SimpleClassHierarchy {
  SimpleClassHierarchy() {
    auto simple_class =
        [](const std::string& name, const DexType* super_type,
           boost::optional<std::pair<std::string, std::string>> field =
               boost::none,
           boost::optional<std::vector<const char*>> method = boost::none,
           boost::optional<DexType*> intf = boost::none,
           boost::optional<DexAccessFlags> access = boost::none) {
          auto this_type = DexType::make_type(name.c_str());
          ClassCreator cc(this_type);
          cc.set_super(const_cast<DexType*>(super_type));
          if (intf) {
            cc.add_interface(*intf);
          }
          if (access) {
            cc.set_access(*access);
          }
          if (field) {
            cc.add_field(
                DexField::make_field(this_type,
                                     DexString::make_string(field->first),
                                     DexType::make_type(field->second.c_str()))
                    ->make_concrete(DexAccessFlags::ACC_PUBLIC));
          }
          if (method) {
            auto args = *method;
            args.erase(args.begin(), args.begin() + 2);
            cc.add_method(DexMethod::make_method(name.c_str(), (*method)[0],
                                                 (*method)[1], args)
                              ->make_concrete(DexAccessFlags::ACC_PUBLIC |
                                                  DexAccessFlags::ACC_ABSTRACT,
                                              /*is_virtual=*/true));
          }
          return cc.create();
        };
    foo = simple_class("LFoo;", type::java_lang_Throwable());
    xyzzy = simple_class("LXyzzy;", type::java_lang_Object());
    bar = simple_class(
        "LBar;", foo->get_type(),
        std::make_pair<std::string, std::string>("m_xyzzy", "LXyzzy;"));
    baz = simple_class(
        "LBaz;", bar->get_type(), /*field=*/boost::none,
        std::vector<const char*>{"methodBar", "LXyzzy;", "LXyzzy;"});
    qux = simple_class("LQux;", baz->get_type());
    iquux =
        simple_class("LIQuux;", type::java_lang_Object(), /*field=*/boost::none,
                     /*method=*/boost::none,
                     /*intf=*/boost::none, DexAccessFlags::ACC_INTERFACE);
    quuz = simple_class("LQuuz;", foo->get_type(), /*field=*/boost::none,
                        /*method=*/boost::none, iquux->get_type());
  }

  // Will be created in constructor. Hierarchy is:
  //
  // Object -> Throwable -> Foo -> Bar -> Baz -> Qux
  //        -> Xyzzy         |
  //                         |
  //               IQuux -> Quuz
  //
  // Bar has a field of type Xyzzy.
  // Baz has a method with return type Xyzzy and argument type Xyzzy.

  DexClass* foo = nullptr;
  DexClass* bar = nullptr;
  DexClass* baz = nullptr;
  DexClass* qux = nullptr;
  DexClass* iquux = nullptr;
  DexClass* quuz = nullptr;
  DexClass* xyzzy = nullptr;
};

} // namespace test
} // namespace redex
