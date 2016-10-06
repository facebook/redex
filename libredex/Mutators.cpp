/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexUtil.h"
#include "Mutators.h"

namespace mutators {

void make_static(DexMethod* method) {
  auto proto = method->get_proto();
  auto params = proto->get_args()->get_type_list();
  auto clstype = method->get_class();
  // make `this` an explicit parameter
  params.push_front(clstype);
  auto new_args = DexTypeList::make_type_list(std::move(params));
  auto new_proto = DexProto::make_proto(proto->get_rtype(), new_args);
  DexMethodRef ref;
  ref.proto = new_proto;
  method->change(ref, true /* rename_on_collision */);
  method->set_access(method->get_access() | ACC_STATIC);

  // changing the method proto means that we need to change its position in the
  // dmethod list
  auto cls = type_class(clstype);
  cls->remove_method(method);
  method->set_virtual(false);
  cls->add_method(method);
}

}
