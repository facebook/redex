/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

// clang-format off
#define ACCESSFLAGS                         \
  AF(PUBLIC,       public,           0x1)   \
  AF(PRIVATE,      private,          0x2)   \
  AF(PROTECTED,    protected,        0x4)   \
  AF(STATIC,       static,           0x8)   \
  AF(FINAL,        final,           0x10)   \
  AF(SYNCHRONIZED, synchronized,    0x20)   \
  AF(VOLATILE,     volatile,        0x40)   \
  AF(BRIDGE,       bridge,          0x40)   \
  AF(TRANSIENT,    transient,       0x80)   \
  AF(VARARGS,      varargs,         0x80)   \
  AF(NATIVE,       native,         0x100)   \
  AF(INTERFACE,    interface,      0x200)   \
  AF(ABSTRACT,     abstract,       0x400)   \
  AF(STRICT,       strict,         0x800)   \
  AF(SYNTHETIC,    synthetic,     0x1000)   \
  AF(ANNOTATION,   annotation,    0x2000)   \
  AF(ENUM,         enum,          0x4000)   \
  AF(CONSTRUCTOR,  constructor,  0x10000)   \
  AF(DECLARED_SYNCHRONIZED, declared_synchronized, 0x2000)
// clang-format on

enum DexAccessFlags : uint32_t {
#define AF(uc, lc, val) ACC_##uc = val,
  ACCESSFLAGS
#undef AF
};

inline DexAccessFlags operator&(const DexAccessFlags a,
                                const DexAccessFlags b) {
  return (DexAccessFlags)((uint32_t)a & (uint32_t)b);
}

inline DexAccessFlags operator|(const DexAccessFlags a,
                                const DexAccessFlags b) {
  return (DexAccessFlags)((uint32_t)a | (uint32_t)b);
}

inline DexAccessFlags& operator|=(DexAccessFlags& a, const DexAccessFlags b) {
  a = a | b;
  return a;
}

inline DexAccessFlags operator~(const DexAccessFlags a) {
  return (DexAccessFlags)(~(uint32_t)a);
}

#define AF(uc, lc, val)                       \
  inline bool is_##lc(DexAccessFlags flags) { \
    return (flags & ACC_##uc) == ACC_##uc;    \
  }                                           \
                                              \
  template <class DexMember>                  \
  bool is_##lc(DexMember* m) {                \
    return is_##lc(m->get_access());          \
  }
ACCESSFLAGS
#undef AF

//
// DexAccessFlags visibility accessors
//

const DexAccessFlags VISIBILITY_MASK =
    static_cast<DexAccessFlags>(ACC_PUBLIC | ACC_PRIVATE | ACC_PROTECTED);

inline bool is_package_private(DexAccessFlags flags) {
  return (flags & VISIBILITY_MASK) == 0;
}

template <class DexMember>
bool is_package_private(DexMember* m) {
  return is_package_private(m->get_access());
}

class DexClass;
using DexClasses = std::vector<DexClass*>;

/**
 * Loosen those access modifiers of a class that do not require a corresponding
 * change in opcodes. 0. Direct instance methods will not be changed so we don't
 * need update opcodes.
 * 1. Make the class public.
 * 2. Make protected and package-private virtual methods public.
 * 3. Make constructors and static methods public.
 * 4. Make all fields public.
 */
void loosen_access_modifier(DexClass* clazz);

/*
 * Loosen access modifier of classes and @InnerClass annotations without needing
 * change opcodes.
 */
void loosen_access_modifier(const DexClasses& clazz);

template <class DexMember>
void set_public(DexMember* m) {
  m->set_access((m->get_access() & ~VISIBILITY_MASK) | ACC_PUBLIC);
}

template <class DexMember>
void set_private(DexMember* m) {
  m->set_access((m->get_access() & ~VISIBILITY_MASK) | ACC_PRIVATE);
}

template <class DexMember>
void set_final(DexMember* m) {
  m->set_access(m->get_access() | ACC_FINAL);
}

template <class DexMember>
void set_public_final(DexMember* m) {
  m->set_access((m->get_access() & ~VISIBILITY_MASK) | ACC_PUBLIC | ACC_FINAL);
}

inline bool check_required_access_flags(const DexAccessFlags required_set,
                                        const DexAccessFlags access_flags) {
  const DexAccessFlags access_mask = ACC_PUBLIC | ACC_PRIVATE | ACC_PROTECTED;
  const DexAccessFlags required_set_flags = required_set & ~access_mask;
  const DexAccessFlags required_one_set_flags = required_set & access_mask;
  return (required_set_flags & ~access_flags) == 0 &&
         (required_one_set_flags == 0 ||
          (required_one_set_flags & access_flags) != 0);
}

inline bool check_required_unset_access_flags(
    const DexAccessFlags required_unset, const DexAccessFlags access_flags) {
  return (required_unset & access_flags) == 0;
}

inline bool access_matches(const DexAccessFlags required_set,
                           const DexAccessFlags required_unset,
                           const DexAccessFlags access_flags) {
  return check_required_access_flags(required_set, access_flags) &&
         check_required_unset_access_flags(required_unset, access_flags);
}
