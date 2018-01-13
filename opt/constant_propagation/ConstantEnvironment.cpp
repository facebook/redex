/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ConstantEnvironment.h"

int64_t SignedConstantDomain::max_element() const {
  if (constant_domain().is_value()) {
    return *constant_domain().get_constant();
  }
  switch (interval()) {
  case sign_domain::Interval::EMPTY:
    always_assert_log(false, "Empty interval does not have a max element");
  case sign_domain::Interval::EQZ:
  case sign_domain::Interval::LEZ:
    return 0;
  case sign_domain::Interval::LTZ:
    return -1;
  case sign_domain::Interval::GEZ:
  case sign_domain::Interval::GTZ:
  case sign_domain::Interval::ALL:
    return std::numeric_limits<int64_t>::max();
  case sign_domain::Interval::SIZE:
    not_reached();
  }
}

int64_t SignedConstantDomain::min_element() const {
  if (constant_domain().is_value()) {
    return *constant_domain().get_constant();
  }
  switch (interval()) {
  case sign_domain::Interval::EMPTY:
    always_assert_log(false, "Empty interval does not have a min element");
  case sign_domain::Interval::EQZ:
  case sign_domain::Interval::GEZ:
    return 0;
  case sign_domain::Interval::GTZ:
    return 1;
  case sign_domain::Interval::LEZ:
  case sign_domain::Interval::LTZ:
  case sign_domain::Interval::ALL:
    return std::numeric_limits<int64_t>::min();
  case sign_domain::Interval::SIZE:
    not_reached();
  }
}
