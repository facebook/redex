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
  case ConstantValue::ConstantType::WIDE_A: {
    o << "WIDE_A";
    break;
  }
  case ConstantValue::ConstantType::WIDE_B: {
    o << "WIDE_B";
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
                                                 uint16_t first_reg,
                                                 int64_t value) {
  int32_t first_half = (int32_t)((value >> 32) & 0xFFFFFFFFL);
  int32_t second_half = (int32_t)(value & 0xFFFFFFFFL);
  env.set(
      first_reg,
      ConstantDomain::value(first_half, ConstantValue::ConstantType::WIDE_A));
  env.set(
      first_reg + 1,
      ConstantDomain::value(second_half, ConstantValue::ConstantType::WIDE_B));
  return env;
}

ConstPropEnvironment& ConstPropEnvUtil::set_top(ConstPropEnvironment& env,
                                                uint16_t first_reg,
                                                bool is_wide) {
  env.set(first_reg, ConstantDomain::top());
  if (is_wide) {
    env.set(first_reg + 1, ConstantDomain::top());
  }
  return env;
}

bool ConstPropEnvUtil::is_narrow_constant(const ConstPropEnvironment& env,
                                          int16_t reg) {
  const auto& domain = env.get(reg);
  return domain.is_value() &&
         domain.value().type() == ConstantValue::ConstantType::NARROW;
}

bool ConstPropEnvUtil::is_wide_constant(const ConstPropEnvironment& env,
                                        int16_t first_reg) {
  const auto& domain1 = env.get(first_reg);
  const auto& domain2 = env.get(first_reg + 1);
  return domain1.is_value() && domain2.is_value() &&
         domain1.value().type() == ConstantValue::ConstantType::WIDE_A &&
         domain2.value().type() == ConstantValue::ConstantType::WIDE_B;
}

int32_t ConstPropEnvUtil::get_narrow(const ConstPropEnvironment& env,
                                     int16_t reg) {
  assert(is_narrow_constant(env, reg));
  return env.get(reg).value().constant();
}

int64_t ConstPropEnvUtil::get_wide(const ConstPropEnvironment& env,
                                   int16_t first_reg) {
  assert(is_wide_constant(env, first_reg));
  const auto& domain1 = env.get(first_reg);
  const auto& domain2 = env.get(first_reg + 1);

  int64_t result =
      static_cast<int64_t>(domain1.value().constant()) & 0xFFFFFFFFL;
  result <<= 32;
  result |= static_cast<int64_t>(domain2.value().constant()) & 0xFFFFFFFFL;
  return result;
}
