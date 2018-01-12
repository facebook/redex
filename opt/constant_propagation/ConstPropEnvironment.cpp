/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ConstPropEnvironment.h"

std::ostream& operator<<(std::ostream& o, const ConstantValue& cv) {
  o << "ConstantValue[Type:";
  switch (cv.type()) {
  case ConstantValue::ConstantType::NARROW: {
    o << "NARROW";
    break;
  }
  case ConstantValue::ConstantType::WIDE: {
    o << "WIDE";
    break;
  }
  case ConstantValue::ConstantType::INVALID: {
    o << "<INVALID>";
    break;
  }
  }
  o << ", Value: " << cv.constant() << "]";
  return o;
}

std::ostream& operator<<(std::ostream& o, const ConstantDomain& cd) {
  if (cd.is_bottom()) {
    o << "_|_";
  } else if (cd.is_top()) {
    o << "T";
  } else {
    o << cd.value();
  }
  return o;
}

std::string ConstantDomain::str() const {
  std::ostringstream ss;
  ss << *this;
  return ss.str();
}

int64_t SignedConstantDomain::max_element() const {
  if (constant_domain().is_value()) {
    return constant_domain().value().constant();
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
    return constant_domain().value().constant();
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

ConstPropEnvironment& ConstPropEnvUtil::set_narrow(ConstPropEnvironment& env,
                                                   uint16_t reg,
                                                   int32_t value) {
  env.set(reg,
          SignedConstantDomain(value, ConstantValue::ConstantType::NARROW));
  return env;
}

ConstPropEnvironment& ConstPropEnvUtil::set_wide(ConstPropEnvironment& env,
                                                 uint16_t reg,
                                                 int64_t value) {
  env.set(reg, SignedConstantDomain(value, ConstantValue::ConstantType::WIDE));
  return env;
}

ConstPropEnvironment& ConstPropEnvUtil::set_top(ConstPropEnvironment& env,
                                                uint16_t reg,
                                                bool is_wide) {
  env.set(reg, SignedConstantDomain::top());
  return env;
}

bool ConstPropEnvUtil::is_narrow_constant(const ConstPropEnvironment& env,
                                          int16_t reg) {
  const auto& constant_domain = env.get(reg).constant_domain();
  return constant_domain.is_value() &&
         constant_domain.value().type() == ConstantValue::ConstantType::NARROW;
}

bool ConstPropEnvUtil::is_wide_constant(const ConstPropEnvironment& env,
                                        int16_t reg) {
  const auto& constant_domain = env.get(reg).constant_domain();
  return constant_domain.is_value() &&
         constant_domain.value().type() == ConstantValue::ConstantType::WIDE;
}

int32_t ConstPropEnvUtil::get_narrow(const ConstPropEnvironment& env,
                                     int16_t reg) {
  assert(is_narrow_constant(env, reg));
  return env.get(reg).constant_domain().value().constant();
}

int64_t ConstPropEnvUtil::get_wide(const ConstPropEnvironment& env,
                                   int16_t reg) {
  assert(is_wide_constant(env, reg));
  return env.get(reg).constant_domain().value().constant();
}
