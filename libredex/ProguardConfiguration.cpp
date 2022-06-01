/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ProguardConfiguration.h"

#include <algorithm>
#include <boost/functional/hash.hpp>

#include "StlUtil.h"

namespace keep_rules {

bool operator==(const MemberSpecification& lhs,
                const MemberSpecification& rhs) {
  return lhs.requiredSetAccessFlags == rhs.requiredSetAccessFlags &&
         lhs.requiredUnsetAccessFlags == rhs.requiredUnsetAccessFlags &&
         lhs.annotationType == rhs.annotationType && lhs.name == rhs.name &&
         lhs.descriptor == rhs.descriptor;
}

size_t hash_value(const MemberSpecification& spec) {
  size_t seed{0};
  boost::hash_combine(seed, spec.requiredSetAccessFlags);
  boost::hash_combine(seed, spec.requiredUnsetAccessFlags);
  boost::hash_combine(seed, spec.annotationType);
  boost::hash_combine(seed, spec.name);
  boost::hash_combine(seed, spec.descriptor);
  return seed;
}

bool operator==(const ClassSpecification& lhs, const ClassSpecification& rhs) {
  return lhs.classNames == rhs.classNames &&
         lhs.annotationType == rhs.annotationType &&
         lhs.extendsClassName == rhs.extendsClassName &&
         lhs.extendsAnnotationType == rhs.extendsAnnotationType &&
         lhs.setAccessFlags == rhs.setAccessFlags &&
         lhs.unsetAccessFlags == rhs.unsetAccessFlags &&
         lhs.fieldSpecifications == rhs.fieldSpecifications &&
         lhs.methodSpecifications == rhs.methodSpecifications;
}

size_t hash_value(const ClassSpecification& spec) {
  size_t seed{0};
  boost::hash_combine(seed, spec.classNames);
  boost::hash_combine(seed, spec.annotationType);
  boost::hash_combine(seed, spec.extendsClassName);
  boost::hash_combine(seed, spec.extendsAnnotationType);
  boost::hash_combine(seed, spec.setAccessFlags);
  boost::hash_combine(seed, spec.unsetAccessFlags);
  boost::hash_combine(seed, spec.fieldSpecifications);
  boost::hash_combine(seed, spec.methodSpecifications);
  return seed;
}

bool operator==(const KeepSpec& lhs, const KeepSpec& rhs) {
  return lhs.includedescriptorclasses == rhs.includedescriptorclasses &&
         lhs.allowshrinking == rhs.allowshrinking &&
         lhs.allowoptimization == rhs.allowoptimization &&
         lhs.allowobfuscation == rhs.allowobfuscation &&
         lhs.class_spec == rhs.class_spec;
}

size_t hash_value(const KeepSpec& spec) {
  size_t seed{0};
  boost::hash_combine(seed, spec.includedescriptorclasses);
  boost::hash_combine(seed, spec.allowshrinking);
  boost::hash_combine(seed, spec.allowoptimization);
  boost::hash_combine(seed, spec.allowobfuscation);
  boost::hash_combine(seed, spec.class_spec);
  return seed;
}

void KeepSpecSet::erase_if(const std::function<bool(const KeepSpec&)>& pred) {
  std::unordered_set<const KeepSpec*> erased;
  std20::erase_if(m_unordered_set, [&](auto& v) {
    if (pred(*v)) {
      erased.emplace(v.get());
      return true;
    }
    return false;
  });
  m_ordered.erase(
      std::remove_if(m_ordered.begin(),
                     m_ordered.end(),
                     [&](const KeepSpec* ks) { return erased.count(ks); }),
      m_ordered.end());
}

} // namespace keep_rules
