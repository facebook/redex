/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "GlobalConstProp.h"

std::ostream& operator<<(std::ostream& o, const ConstantValue& cv) {
  o << "ConstantValue[ Type:";
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

ConstPropEnvironment& ConstPropEnvUtil::set_narrow(ConstPropEnvironment& env,
                                                   uint16_t reg,
                                                   int32_t value) {
  env.set(reg,
          ConstantDomain::value(value, ConstantValue::ConstantType::NARROW));
  return env;
}

ConstPropEnvironment& ConstPropEnvUtil::set_wide(ConstPropEnvironment& env,
                                                 uint16_t reg,
                                                 int64_t value) {
  env.set(
      reg,
      ConstantDomain::value(value, ConstantValue::ConstantType::WIDE));
  return env;
}

ConstPropEnvironment& ConstPropEnvUtil::set_top(ConstPropEnvironment& env,
                                                uint16_t reg,
                                                bool is_wide) {
  env.set(reg, ConstantDomain::top());
  return env;
}

bool ConstPropEnvUtil::is_narrow_constant(const ConstPropEnvironment& env,
                                          int16_t reg) {
  const auto& domain = env.get(reg);
  return domain.is_value() &&
         domain.value().type() == ConstantValue::ConstantType::NARROW;
}

bool ConstPropEnvUtil::is_wide_constant(const ConstPropEnvironment& env,
                                        int16_t reg) {
  const auto& domain = env.get(reg);
  return domain.is_value() &&
         domain.value().type() == ConstantValue::ConstantType::WIDE;
}

int32_t ConstPropEnvUtil::get_narrow(const ConstPropEnvironment& env,
                                     int16_t reg) {
  assert(is_narrow_constant(env, reg));
  return env.get(reg).value().constant();
}

int64_t ConstPropEnvUtil::get_wide(const ConstPropEnvironment& env,
                                   int16_t reg) {
  assert(is_wide_constant(env, reg));
  return env.get(reg).value().constant();
}
