/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <type_traits>
#include <vector>

#include "DexAccess.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "MethodUtil.h"
#include "ReachableClasses.h"
#include "Resolver.h"

namespace m {

namespace detail {

/**
 * is_rvalue<T> is only instantiated for types T that are rvalue references.
 * Used to restrict a signature with a forwarding reference so it does not
 * accept an lvalue reference.
 */
template <typename T>
using is_rvalue =
    typename std::enable_if<std::is_rvalue_reference<T&&>::value>::type;

bool is_assignable_to(const DexType* child, const DexType* parent);
bool is_default_constructor(const DexMethod* meth);

} // namespace detail

// N.B. recursive template for matching opcode pattern against insn sequence
template <typename T, typename N>
struct insns_matcher {
  static bool matches_at(int at,
                         const std::vector<IRInstruction*>& insns,
                         const T& t) {
    const auto& insn = insns.at(at);
    typename std::tuple_element<N::value, T>::type insn_match =
        std::get<N::value>(t);
    return insn_match.matches(insn) &&
           insns_matcher<T, std::integral_constant<size_t, N::value + 1>>::
               matches_at(at + 1, insns, t);
  }
};

// N.B. base case of recursive template where N = opcode pattern length
template <typename T>
struct insns_matcher<
    T,
    std::integral_constant<size_t, std::tuple_size<T>::value>> {
  static bool matches_at(int at,
                         const std::vector<IRInstruction*>& insns,
                         const T& t) {
    return true;
  }
};

// Find all sequences in `insns` that match `p` and put them into `matches`
template <typename P, size_t N = std::tuple_size<P>::value>
void find_matches(const std::vector<IRInstruction*>& insns,
                  const P& p,
                  std::vector<std::vector<IRInstruction*>>& matches) {
  // No way to match if we have fewer insns than N
  if (insns.size() >= N) {
    // Try to match starting at i
    for (size_t i = 0; i <= insns.size() - N; ++i) {
      if (m::insns_matcher<P, std::integral_constant<size_t, 0>>::matches_at(
              i, insns, p)) {
        matches.emplace_back();
        auto& matching_insns = matches.back();
        matching_insns.reserve(N);
        for (size_t c = 0; c < N; ++c) {
          matching_insns.push_back(insns.at(i + c));
        }
      }
    }
  }
}

// Find all instructions in `insns` that match `p`
template <typename P>
void find_insn_match(const std::vector<IRInstruction*>& insns,
                     const P& p,
                     std::vector<IRInstruction*>& matches) {
  for (auto insn : insns) {
    if (p.matches(insn)) {
      matches.emplace_back(insn);
    }
  }
}

/**
 * Maps domain of matching predicate to the type expected by `matches`. Provides
 * an immutable type -- `val` -- use as a parameter, and its mutable equivalent
 * -- `var` -- for convenience.
 *
 * By default, the matching predicate on `T` expects a reference to `T`.
 */
template <typename T>
struct arg {
  using val = const T&;
  using var = T&;
};

/**
 * Specialisation for predicates on pointer types.  Expects the pointer
 * directly, rather than a reference to it, to save an indirection.
 */
template <typename T>
struct arg<T*> {
  using val = const T*;
  using var = T*;
};

/**
 * Zero cost wrapper over a callable type with the following signature:
 *
 *   (const T*) const -> bool
 *
 * The resulting object can be used with the combinators defined in this
 * header to form more complex predicates.  This wrapper serves two purposes:
 *
 * - It prevents the combinators defined below from interfering with the
 *   overload resolution for any callable object -- they must be opted-in by
 *   wrapping.
 * - It allows us to use template deduction to hide the implementation of the
 *   predicate (the second template parameter), while still constraining over
 *   the type being matched over.
 */
template <typename T, typename P>
struct match_t {
  using arg_type = typename arg<T>::val;

  explicit match_t(P fn) : m_fn(std::move(fn)) {}
  bool matches(arg_type t) const { return m_fn(t); }

 private:
  P m_fn;
};

/**
 * Create a match_t from a matching function, fn, of type `const T* -> bool`.
 * Supports template deduction so lambdas can be wrapped without referring to
 * their type (which cannot be easily named).
 */
template <typename T, typename P>
inline match_t<T, P> matcher(P fn) {
  return match_t<T, P>(std::move(fn));
}

/** Match a subordinate match whose logical not is true */
template <typename T, typename P>
inline auto operator!(match_t<T, P> p) {
  using arg_type = typename arg<T>::val;
  return matcher<T>([p = std::move(p)](arg_type t) { return !p.matches(t); });
}

/** Match two subordinate matches whose logical or is true */
template <typename T, typename P, typename Q>
inline auto operator||(match_t<T, P> p, match_t<T, Q> q) {
  using arg_type = typename arg<T>::val;
  return matcher<T>([p = std::move(p), q = std::move(q)](arg_type t) {
    return p.matches(t) || q.matches(t);
  });
}

/** Match two subordinate matches whose logical and is true */
template <typename T, typename P, typename Q>
inline auto operator&&(match_t<T, P> p, match_t<T, Q> q) {
  using arg_type = typename arg<T>::val;
  return matcher<T>([p = std::move(p), q = std::move(q)](arg_type t) {
    return p.matches(t) && q.matches(t);
  });
}

/** Match two subordinate matches whose logical xor is true */
template <typename T, typename P, typename Q>
inline auto operator^(match_t<T, P> p, match_t<T, Q> q) {
  using arg_type = typename arg<T>::val;
  return matcher<T>([p = std::move(p), q = std::move(q)](arg_type t) {
    return p.matches(t) ^ q.matches(t);
  });
}

/** Match any T (always matches) */
template <typename T>
inline auto any() {
  using arg_type = typename arg<T>::val;
  return matcher<T>([](arg_type) { return true; });
}

/**
 * Equality predicates
 *
 * If equals is passed an lvalue reference, it assumes that the referent lives
 * longer than the returned matcher.
 *
 * If an rvalue reference is passed in, the returned matcher will take ownership
 * of the temporary.
 *
 * Pointers are special cased to be compared by value, to avoid an indirection.
 */
template <typename T>
inline auto equals(const T& expect) {
  return matcher<T>([&expect](const T& actual) { return expect == actual; });
}

template <typename T, typename = detail::is_rvalue<T>>
inline auto equals(T&& expect) {
  return matcher<T>([expect = std::forward<T>(expect)](const T& actual) {
    return expect == actual;
  });
}

template <typename T>
inline auto equals(T* expect) {
  return matcher<T*>([expect](const T* actual) { return expect == actual; });
}

template <typename T>
inline auto equals(const T* expect) {
  return matcher<T*>([expect](const T* actual) { return expect == actual; });
}

/**
 * Match which checks for membership of T in container C via C::find(T)
 *
 * If an lvalue reference is passed in, it is assumed that the referent lives
 * longer than the returned matcher.
 *
 * If the container is passed in by rvalue reference, the returned matcher will
 * take ownership of the temporary.
 */
template <typename T, typename C>
inline auto in(const C& c) {
  using arg_type = typename arg<T>::val;
  using mut_type = typename arg<T>::var;
  return matcher<T>(
      [&c](arg_type t) { return c.find(const_cast<mut_type>(t)) != c.end(); });
}

template <typename T, typename C, typename = detail::is_rvalue<C>>
inline auto in(C&& c) {
  using arg_type = typename arg<T>::val;
  using mut_type = typename arg<T>::var;
  return matcher<T>([c = std::forward<C>(c)](arg_type t) {
    return c.find(const_cast<mut_type>(t)) != c.end();
  });
}

/** Match any T named thusly */
template <typename T>
inline auto named(const char* name) {
  return matcher<T*>(
      [name](const T* t) { return t->get_name()->str() == name; });
}

/** Matching on a type's access flags */
template <typename T, typename P>
inline auto access(match_t<DexAccessFlags, P> p) {
  return matcher<T*>(
      [p = std::move(p)](const T* t) { return p.matches(t->get_access()); });
}

/**
 * For each {access} flag, a matcher with the following signature:
 *
 *   template <typename T> match_t<T*> is_{access}()
 *
 * Where `T` is a type with a `get_access()` member function.
 */
#define AF(_0, ACCESS, _2)                                          \
  template <typename T>                                             \
  inline auto is_##ACCESS() {                                       \
    return matcher<T*>(                                             \
        [](const T* t) { return ::is_##ACCESS(t->get_access()); }); \
  }
ACCESSFLAGS
#undef AF

/**
 * For each opcode and pseudo-opcode two matchers with the
 * following signatures:
 *
 *   template <typename P>
 *   match_t<IRInstruction*> {pred}_(match_t<IRInstruction*, P>)
 *
 * Matches instructions whose opcode matches {pred} and additionally matches
 * the subordinate matcher.
 *
 *   match_t<IRInstruction*> {pred}_()
 *
 * Matches instructions whose opcode satisfies {pred}.  Note that the matcher's
 * name has a trailing underscore to protect against opcodes that are also
 * C++ keywords.
 */
#define OPCODE_MATCHER(PRED)                                           \
  template <typename P>                                                \
  inline auto PRED##_(match_t<IRInstruction*, P> p) {                  \
    return matcher<IRInstruction*>(                                    \
        [p = std::move(p)](const IRInstruction* insn) {                \
          return opcode::is_##PRED(insn->opcode()) && p.matches(insn); \
        });                                                            \
  }                                                                    \
                                                                       \
  inline auto PRED##_() { return PRED##_(any<IRInstruction*>()); }

#define OP(_, LC, ...) OPCODE_MATCHER(LC)
#define IOP(_, LC, ...) OPCODE_MATCHER(LC)

/*
 * For each oprange, generates two matchers:
 *
 *   template <typename P>
 *   match_t<IRInstruction*> {range}(match_t<IRInstruction*, P>)
 *
 * Matches instructions whose opcode is in {range} and additionally matches
 * the subordinate matcher.
 *
 *   match_t<IRInstruction*> {range}()
 *
 * Matches instructions whose opcode is in {range}.
 */
#define OPRANGE(NAME, ...)                                             \
  template <typename P>                                                \
  inline auto NAME(match_t<IRInstruction*, P> p) {                     \
    return matcher<IRInstruction*>(                                    \
        [p = std::move(p)](const IRInstruction* insn) {                \
          return opcode::is_##NAME(insn->opcode()) && p.matches(insn); \
        });                                                            \
  }                                                                    \
                                                                       \
  inline auto NAME() { return NAME(any<IRInstruction*>()); }

#include "IROpcodes.def"
#undef OPCODE_MATCHER

/** Match T's which are external */
template <typename T>
inline auto is_external() {
  return matcher<T*>([](const T* t) { return t->is_external(); });
}

/** Match methods that are default constructors */
inline auto is_default_constructor() {
  return matcher<DexMethod*>(detail::is_default_constructor);
}

inline auto can_be_default_constructor() {
  return matcher<DexMethodRef*>([](const DexMethodRef* meth) {
    const DexMethod* def = meth->as_def();
    return def && detail::is_default_constructor(def);
  });
}

/** Match methods that are constructors. INCLUDES static constructors! */
inline auto can_be_constructor() {
  return matcher<DexMethodRef*>(
      [](const DexMethodRef* meth) { return method::is_constructor(meth); });
}

/** Matches instructions with specified number of arguments. */
inline auto has_n_args(size_t n) {
  return matcher<IRInstruction*>(
      [n](const IRInstruction* insn) { return insn->srcs_size() == n; });
}

/** Matchers that map from IRInstruction -> other types */
template <typename P>
inline auto has_method(match_t<DexMethodRef*, P> p) {
  return matcher<IRInstruction*>([p = std::move(p)](const IRInstruction* insn) {
    return insn->has_method() && p.matches(insn->get_method());
  });
}

inline auto has_method() { return has_method(any<DexMethodRef*>()); }

template <typename P>
inline auto has_field(match_t<DexFieldRef*, P> p) {
  return matcher<IRInstruction*>([p = std::move(p)](const IRInstruction* insn) {
    return insn->has_field() && p.matches(insn->get_field());
  });
}

inline auto has_field() { return has_field(any<DexFieldRef*>()); }

template <typename P>
inline auto has_type(match_t<DexType*, P> p) {
  return matcher<IRInstruction*>([p = std::move(p)](const IRInstruction* insn) {
    return insn->has_type() && p.matches(insn->get_type());
  });
}

inline auto has_type() { return has_type(any<DexType*>()); }

template <typename P>
inline auto has_string(match_t<DexString*, P> p) {
  return matcher<IRInstruction*>([p = std::move(p)](const IRInstruction* insn) {
    return insn->has_string() && p.matches(insn->get_string());
  });
}

inline auto has_string() { return has_string(any<DexString*>()); }

template <typename P>
inline auto has_literal(match_t<int64_t, P> p) {
  return matcher<IRInstruction*>([p = std::move(p)](const IRInstruction* insn) {
    return insn->has_literal() && p.matches(insn->get_literal());
  });
}

inline auto has_literal() { return has_literal(any<int64_t>()); }

/** Match types which can be assigned to the given type */
inline auto is_assignable_to(const DexType* parent) {
  return matcher<DexType*>([parent](const DexType* child) {
    return detail::is_assignable_to(child, parent);
  });
}

/** Match members and check predicate on their type */
template <typename Member, typename P>
inline auto member_of(match_t<DexType*, P> p) {
  return matcher<Member*>([p = std::move(p)](const Member* member) {
    return p.matches(member->get_class());
  });
}

/** Predicate on a method after it is resolved. */
template <typename P>
inline auto resolve_method(MethodSearch ms, match_t<DexMethod*, P> p) {
  return matcher<DexMethodRef*>([ms, p = std::move(p)](const DexMethodRef* mr) {
    // resolve_method accepts a non-const DexMethodRef* to return a non-const
    // DexMethod*.  const_cast is safe to get around that as the return value
    // is treated as const.
    const auto* m = resolve_method(const_cast<DexMethodRef*>(mr), ms);
    return m && p.matches(m);
  });
}

/** Match classes that have class data */
inline auto has_class_data() {
  return matcher<DexClass*>(
      [](const DexClass* cls) { return cls->has_class_data(); });
}

/** Match classes satisfying the given method match for any vmethods */
template <typename P>
inline auto any_vmethods(match_t<DexMethod*, P> p) {
  return matcher<DexClass*>([p = std::move(p)](const DexClass* cls) {
    const auto& vmethods = cls->get_vmethods();
    return std::any_of(vmethods.begin(),
                       vmethods.end(),
                       [&p](const DexMethod* meth) { return p.matches(meth); });
  });
}

/** Match classes satisfying the given method match for any dmethods */
template <typename P>
inline auto any_dmethods(match_t<DexMethod*, P> p) {
  return matcher<DexClass*>([p = std::move(p)](const DexClass* cls) {
    const auto& dmethods = cls->get_dmethods();
    return std::any_of(dmethods.begin(),
                       dmethods.end(),
                       [&p](const DexMethod* meth) { return p.matches(meth); });
  });
}

/** Match classes satisfying the given field match for any ifields */
template <typename P>
inline auto any_ifields(match_t<DexField*, P> p) {
  return matcher<DexClass*>([p = std::move(p)](const DexClass* cls) {
    const auto& ifields = cls->get_ifields();
    return std::any_of(ifields.begin(),
                       ifields.end(),
                       [&p](const DexField* meth) { return p.matches(meth); });
  });
}

/** Match classes satisfying the given field match for any sfields */
template <typename P>
inline auto any_sfields(match_t<DexField*, P> p) {
  return matcher<DexClass*>([p = std::move(p)](const DexClass* cls) {
    const auto& sfields = cls->get_sfields();
    return std::any_of(sfields.begin(),
                       sfields.end(),
                       [&p](const DexField* meth) { return p.matches(meth); });
  });
}

/** Match dex members containing any annotation that matches the given match */
template <typename T, typename P>
inline auto any_annos(match_t<DexAnnotation*, P> p) {
  return matcher<T*>([p = std::move(p)](const T* t) {
    if (!t->is_def()) {
      return false;
    }

    const auto& anno_set = t->get_anno_set();
    if (!anno_set) {
      return false;
    }

    const auto& annos = anno_set->get_annotations();
    return std::any_of(
        annos.begin(), annos.end(), [&p](const DexAnnotation* anno) {
          return p.matches(anno);
        });
  });
}

/**
 * Maps match<T, X> => match<DexType(t), X>
 */
template <typename T, typename P>
inline auto as_type(match_t<DexType*, P> p) {
  return matcher<T*>(
      [p = std::move(p)](const T* t) { return p.matches(t->type()); });
}

/**
 * Maps match<DexType, X> => match<DexClass, X>
 */
template <typename P>
inline auto as_class(match_t<DexClass*, P> p) {
  return matcher<DexType*>([p = std::move(p)](const DexType* t) {
    auto cls = type_class(t);
    return cls && p.matches(cls);
  });
}

/** Match which checks can_delete helper for DexMembers */
template <typename T>
inline auto can_delete() {
  return matcher<T*>(can_delete);
}

/** Match which checks can_rename helper for DexMembers */
template <typename T>
inline auto can_rename() {
  return matcher<T*>(can_rename);
}

/** Match which checks keep helper for DexMembers */
template <typename T>
inline auto has_keep() {
  return matcher<T*>(has_keep);
}

} // namespace m
