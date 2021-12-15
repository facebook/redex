/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantEnvironment.h"

int64_t SignedConstantDomain::max_element() const {
  if (constant_domain().is_value()) {
    return *constant_domain().get_constant();
  }
  auto max = numeric_interval_domain().upper_bound();
  if (max < NumericIntervalDomain::MAX) {
    return max;
  }
  return sign_domain::max_int(interval());
}

int64_t SignedConstantDomain::min_element() const {
  if (constant_domain().is_value()) {
    return *constant_domain().get_constant();
  }
  auto min = numeric_interval_domain().lower_bound();
  if (min > NumericIntervalDomain::MIN) {
    return min;
  }
  return sign_domain::min_int(interval());
}

// TODO: Instead of this custom meet function, the ConstantValue should get a
// custom meet AND JOIN that knows about the relationship of NEZ and certain
// non-null custom object domains.
ConstantValue meet(const ConstantValue& left, const ConstantValue& right) {
  auto is_nez = [](const ConstantValue& value) {
    auto signed_value = value.maybe_get<SignedConstantDomain>();
    return signed_value &&
           signed_value->interval() == sign_domain::Interval::NEZ;
  };
  auto is_not_null = [](const ConstantValue& value) {
    return !value.is_top() && !value.is_bottom() &&
           !value.maybe_get<SignedConstantDomain>();
  };
  // Non-null objects of custom object domains are compatible with NEZ, and
  // more specific.
  if (is_nez(left) && is_not_null(right)) {
    return right;
  }
  if (is_nez(right) && is_not_null(left)) {
    return left;
  }

  // SingletonObjectDomain and ObjectWithImmutAttrDomain both represent object
  // references and they have intersection.
  // Handle their meet operator specially.
  auto is_singleton_obj = [](const ConstantValue& value) {
    auto obj = value.maybe_get<SingletonObjectDomain>();
    return obj;
  };
  auto is_obj_with_immutable_attr = [](const ConstantValue& value) {
    auto obj = value.maybe_get<ObjectWithImmutAttrDomain>();
    return obj;
  };
  if (is_singleton_obj(left) && is_obj_with_immutable_attr(right)) {
    return right;
  }
  if (is_singleton_obj(right) && is_obj_with_immutable_attr(left)) {
    return left;
  }
  // Non-null objects of different custom object domains can never alias, so
  // they meet at bottom, which is the default meet implementation for
  // disjoint domains.
  return left.meet(right);
}
