/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>

#include "DexClass.h"

struct Location;

namespace dispatch {

enum Type {
  // Ctor that takes the generated type tag as the last parameter.
  CTOR_WITH_TYPE_TAG_PARAM,
  CTOR,
  STATIC,
  VIRTUAL,
};

struct Spec {
  DexType* owner_type;
  Type type;
  const std::string name;
  DexProto* proto;
  DexAccessFlags access_flags;
  DexField* type_tag_field;
  DexMethod* overridden_meth;
  boost::optional<size_t> max_num_dispatch_target;
  bool keep_debug_info;

  Spec(DexType* owner_type,
       Type type,
       const std::string name,
       DexProto* proto,
       DexAccessFlags access_flags,
       DexField* type_tag_field,
       DexMethod* overridden_meth,
       bool keep_debug_info)
      : owner_type(owner_type),
        type(type),
        name(name),
        proto(proto),
        access_flags(access_flags),
        type_tag_field(type_tag_field),
        overridden_meth(overridden_meth),
        keep_debug_info(keep_debug_info) {
    max_num_dispatch_target = boost::none;
  }

  Spec(DexType* owner_type,
       Type type,
       const std::string name,
       DexProto* proto,
       DexAccessFlags access_flags,
       DexField* type_tag_field,
       DexMethod* overridden_meth,
       boost::optional<size_t> max_num_dispatch_target,
       bool keep_debug_info)
      : owner_type(owner_type),
        type(type),
        name(name),
        proto(proto),
        access_flags(access_flags),
        type_tag_field(type_tag_field),
        overridden_meth(overridden_meth),
        max_num_dispatch_target(max_num_dispatch_target),
        keep_debug_info(keep_debug_info) {}
};

struct DispatchMethod {
  DexMethod* main_dispatch;
  std::vector<DexMethod*> sub_dispatches;

  explicit DispatchMethod(DexMethod* main) : main_dispatch(main) {}
  DispatchMethod(DexMethod* main, std::vector<DexMethod*> subs)
      : main_dispatch(main), sub_dispatches(subs) {}
};

/**
 * A high level API that assesses the size of the switch dispatch needed and
 * potentially split it when necessary.
 */
dispatch::DispatchMethod create_virtual_dispatch(
    const Spec& spec,
    const std::map<SwitchIndices, DexMethod*>& indices_to_callee);

DexMethod* create_ctor_or_static_dispatch(
    const Spec& spec,
    const std::map<SwitchIndices, DexMethod*>& indices_to_callee);

/**
 * Generate a new dispatch method name.
 */
DexString* gen_dispatch_name(DexType* owner,
                             DexProto* proto,
                             std::string orig_name);

} // namespace dispatch
