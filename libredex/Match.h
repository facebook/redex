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

#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "ReachableClasses.h"

namespace m {

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

/** N-ary match template */
template <typename T,
          typename P = std::tuple<>,
          size_t N = std::tuple_size<P>::value>
struct match_t;

/** Nullary specialization of N-ary match */
template <typename T, typename P>
struct match_t<T, P, 0> {
  bool (*fn)(const T*);
  bool matches(const T* t) const { return fn(t); }
};

/** Unary specialization of N-ary match */
template <typename T, typename P>
struct match_t<T, P, 1> {
  using P0_t = typename std::tuple_element<0, P>::type;
  bool (*fn)(const T*, const P0_t& p0);
  P0_t p0;
  bool matches(const T* t) const { return fn(t, p0); }
};

/** Binary specialization of N-ary match */
template <typename T, typename P>
struct match_t<T, P, 2> {
  using P0_t = typename std::tuple_element<0, P>::type;
  using P1_t = typename std::tuple_element<1, P>::type;
  bool (*fn)(const T*, const P0_t& p0, const P1_t& p1);
  P0_t p0;
  P1_t p1;
  bool matches(const T* t) const { return fn(t, p0, p1); }
};

/** Match a subordinate match whose logical not is true */
template <typename T, typename P0>
match_t<T, std::tuple<match_t<T, P0>>> operator!(const match_t<T, P0>& p0) {
  return {[](const T* t, const match_t<T, P0>& p0) { return !p0.matches(t); },
          p0};
}

/** Match two subordinate matches whose logical or is true */
template <typename T, typename P0, typename P1>
match_t<T, std::tuple<match_t<T, P0>, match_t<T, P1>>> operator||(
    const match_t<T, P0>& p0, const match_t<T, P1>& p1) {
  return {[](const T* t, const match_t<T, P0>& p0, const match_t<T, P1>& p1) {
            return p0.matches(t) || p1.matches(t);
          },
          p0, p1};
}

/** Match two subordinate matches whose logical and is true */
template <typename T, typename P0, typename P1>
match_t<T, std::tuple<match_t<T, P0>, match_t<T, P1>>> operator&&(
    const match_t<T, P0>& p0, const match_t<T, P1>& p1) {
  return {[](const T* t, const match_t<T, P0>& p0, const match_t<T, P1>& p1) {
            return p0.matches(t) && p1.matches(t);
          },
          p0, p1};
}

/** Match two subordinate matches whose logical xor is true */
template <typename T, typename P0, typename P1>
match_t<T, std::tuple<match_t<T, P0>, match_t<T, P1>>> operator^(
    const match_t<T, P0>& p0, const match_t<T, P1>& p1) {
  return {[](const T* t, const match_t<T, P0>& p0, const match_t<T, P1>& p1) {
            return p0.matches(t) ^ p1.matches(t);
          },
          p0, p1};
}

/** Match any T (always matches) */
template <typename T>
match_t<T, std::tuple<>> any() {
  return {[](const T* t) { return true; }};
}

/** Compare two T at pointers */
template <typename T>
m::match_t<T, std::tuple<const T*>> ptr_eq(const T* expected) {
  return {[](const T* actual, const T* const& expected) {
            return actual == expected;
          },
          expected};
}

// N.B. free beer offer to anyone who can get 'named' to work on const char*
// instead of std::string&
/** Match any T named thusly */
template <typename T>
match_t<T, std::tuple<const std::string>> named(const std::string& name) {
  return {[](const T* t, const std::string& name) {
            return !strcmp(t->get_name()->c_str(), name.c_str());
          },
          name};
}

/** Match T's which are external */
template <typename T>
match_t<T, std::tuple<>> is_external() {
  return {[](const T* t) { return t->is_external(); }};
}

/** Match T's which are final */
template <typename T>
match_t<T, std::tuple<>> is_final() {
  return {[](const T* t) { return (bool)(t->get_access() & ACC_FINAL); }};
}

/** Match T's which are static */
template <typename T>
match_t<T, std::tuple<>> is_static() {
  return {[](const T* t) { return (bool)(t->get_access() & ACC_STATIC); }};
}

/** Match T's which are interfaces */
template <typename T>
match_t<T, std::tuple<>> is_abstract() {
  return {[](const T* t) { return (bool)(t->get_access() & ACC_ABSTRACT); }};
}

/** Match types which are interfaces */
match_t<DexClass, std::tuple<>> is_interface();

/**
 * Matches IRInstructions
 */

/** Any instruction which holds a type reference */
match_t<IRInstruction> has_type();

/** const-string flavors */
match_t<IRInstruction> const_string();

/** move-result-pseudo flavors */
match_t<IRInstruction> move_result_pseudo();

/** new-instance flavors */
template <typename P>
match_t<IRInstruction, std::tuple<match_t<IRInstruction, P>>> new_instance(
    const match_t<IRInstruction, P>& p) {
  return {[](const IRInstruction* insn, const match_t<IRInstruction, P>& p) {
            auto opcode = insn->opcode();
            if (opcode == OPCODE_NEW_INSTANCE) {
              return p.matches(insn);
            } else {
              return false;
            }
          },
          p};
}

match_t<IRInstruction, std::tuple<match_t<IRInstruction>>> new_instance();

/** throw flavors */
match_t<IRInstruction> throwex();

/** invoke-direct flavors */
template <typename P>
match_t<IRInstruction, std::tuple<match_t<IRInstruction, P>>> invoke_direct(
    const match_t<IRInstruction, P>& p) {
  return {[](const IRInstruction* insn, const match_t<IRInstruction, P>& p) {
            auto opcode = insn->opcode();
            if (opcode == OPCODE_INVOKE_DIRECT) {
              return p.matches(insn);
            } else {
              return false;
            }
          },
          p};
}

match_t<IRInstruction, std::tuple<match_t<IRInstruction>>> invoke_direct();

/** invoke-static flavors */
template <typename P>
match_t<IRInstruction, std::tuple<match_t<IRInstruction, P>>> invoke_static(
    const match_t<IRInstruction, P>& p) {
  return {[](const IRInstruction* insn, const match_t<IRInstruction, P>& p) {
            auto opcode = insn->opcode();
            if (opcode == OPCODE_INVOKE_STATIC) {
              return p.matches(insn);
            } else {
              return false;
            }
          },
          p};
}

match_t<IRInstruction, std::tuple<match_t<IRInstruction>>> invoke_static();

/** invoke-virtual flavors */
template <typename P>
match_t<IRInstruction, std::tuple<match_t<IRInstruction, P>>> invoke_virtual(
    const match_t<IRInstruction, P>& p) {
  return {[](const IRInstruction* insn, const match_t<IRInstruction, P>& p) {
            auto opcode = insn->opcode();
            if (opcode == OPCODE_INVOKE_VIRTUAL) {
              return p.matches(insn);
            } else {
              return false;
            }
          },
          p};
}

match_t<IRInstruction, std::tuple<match_t<IRInstruction>>> invoke_virtual();

/** invoke of any kind */
template <typename P>
match_t<IRInstruction, std::tuple<match_t<IRInstruction, P>>> invoke(
    const match_t<IRInstruction, P>& p) {
  return {[](const IRInstruction* insn, const match_t<IRInstruction, P>& p) {
            return is_invoke(insn->opcode()) && p.matches(insn);
          },
          p};
}

inline match_t<IRInstruction, std::tuple<match_t<IRInstruction>>> invoke() {
  return invoke(any<IRInstruction>());
};

/** iput flavors */
template <typename P>
match_t<IRInstruction, std::tuple<match_t<IRInstruction, P>>> iput(
    const match_t<IRInstruction, P>& p) {
  return {[](const IRInstruction* insn, const match_t<IRInstruction, P>& p) {
            return is_iput(insn->opcode()) && p.matches(insn);
          },
          p};
}

inline match_t<IRInstruction, std::tuple<match_t<IRInstruction>>> iput() {
  return iput(any<IRInstruction>());
};

/** iget flavors */
template <typename P>
match_t<IRInstruction, std::tuple<match_t<IRInstruction, P>>> iget(
    const match_t<IRInstruction, P>& p) {
  return {[](const IRInstruction* insn, const match_t<IRInstruction, P>& p) {
            return is_iget(insn->opcode()) && p.matches(insn);
          },
          p};
}

inline match_t<IRInstruction, std::tuple<match_t<IRInstruction>>> iget() {
  return iget(any<IRInstruction>());
};

/** return-void */
match_t<IRInstruction> return_void();

/** Matches instructions with specified number of arguments. */
match_t<IRInstruction, std::tuple<size_t>> has_n_args(size_t n);

/** Matches instructions with specified opcode */
inline match_t<IRInstruction, std::tuple<IROpcode>> is_opcode(IROpcode opcode) {
  return {[](const IRInstruction* insn, const IROpcode& opcode) {
            return insn->opcode() == opcode;
          },
          opcode};
}

/** Matchers that map from IRInstruction -> other types */
template <typename P>
match_t<IRInstruction, std::tuple<match_t<DexMethodRef, P>>> opcode_method(
    const match_t<DexMethodRef, P>& predicate) {
  return {[](const IRInstruction* insn, const match_t<DexMethodRef, P>& p) {
            return insn->has_method() && p.matches(insn->get_method());
          },
          predicate};
}

template <typename P>
match_t<IRInstruction, std::tuple<match_t<DexFieldRef, P>>> opcode_field(
    const match_t<DexFieldRef, P>& predicate) {
  return {[](const IRInstruction* insn, const match_t<DexFieldRef, P>& p) {
            return insn->has_field() && p.matches(insn->get_field());
          },
          predicate};
}

template <typename P>
match_t<IRInstruction, std::tuple<match_t<DexType, P>>> opcode_type(
    const match_t<DexType, P>& p) {
  return {[](const IRInstruction* insn, const match_t<DexType, P>& p) {
            return insn->has_type() && p.matches(insn->get_type());
          },
          p};
}

template <typename P>
match_t<IRInstruction, std::tuple<match_t<DexString, P>>> opcode_string(
    const match_t<DexString, P>& p) {
  return {[](const IRInstruction* insn, const match_t<DexString, P>& p) {
            return insn->has_string() && p.matches(insn->get_string());
          },
          p};
}

/** Match types which can be assigned to the given type */
match_t<DexType, std::tuple<const DexType*>> is_assignable_to(
    const DexType* parent);

/** Match methods that are bound to the given class. */
template <typename T>
match_t<T, std::tuple<const std::string>> on_class(const std::string& type) {
  return {[](const T* t, const std::string& type) {
            return !strcmp(t->get_class()->get_name()->c_str(), type.c_str());
          },
          type};
}

/** Match methods whose code satisfied the given opcodes match */
template <typename... T>
match_t<DexMethod, std::tuple<std::tuple<T...>>> has_opcodes(
    const std::tuple<T...>& t) {
  return {[](const DexMethod* meth, const std::tuple<T...>& t) {
            auto code = meth->get_code();
            if (code) {
              const size_t N = std::tuple_size<std::tuple<T...>>::value;
              std::vector<IRInstruction*> insns;
              for (auto& mie : ir_list::InstructionIterable(code)) {
                insns.push_back(mie.insn);
              }
              // No way to match if we have less insns than N
              if (insns.size() < N) {
                return false;
              }
              // Try to match starting at i, we advance along insns until the
              // length of the tuple would cause us to extend beyond the end of
              // insns to make the match.
              for (size_t i = 0; i <= insns.size() - N; ++i) {
                if (insns_matcher<
                        std::tuple<T...>,
                        std::integral_constant<size_t, 0>>::matches_at(i,
                                                                       insns,
                                                                       t)) {
                  return true;
                }
              }
            }
            return false;
          },
          t};
}

/** Match members and check predicate on their type */
template <typename Member, typename P>
match_t<Member, std::tuple<match_t<DexType, P>>> member_of(
    const match_t<DexType, P>& predicate) {
  return {[](const Member* member, const match_t<DexType, P>& p) {
            return p.matches(member->get_class());
          },
          predicate};
}

/** Match methods that are default constructors */
match_t<DexMethod, std::tuple<>> is_default_constructor();
match_t<DexMethodRef, std::tuple<>> can_be_default_constructor();

/** Match methods that are constructors. INCLUDES static constructors! */
match_t<DexMethod, std::tuple<>> is_constructor();
match_t<DexMethodRef, std::tuple<>> can_be_constructor();

/** Match classes that are enums */
match_t<DexClass, std::tuple<>> is_enum();

/** Match classes that have class data */
match_t<DexClass, std::tuple<>> has_class_data();

/** Match classes satisfying the given method match for any vmethods */
template <typename P>
match_t<DexClass, std::tuple<match_t<DexMethod, P>>>
// N.B. Free beer offer to anyone who can figure out how to get the
// default argument below to work. It seems to somehow throw
// off the inference of P at the call/template instantiation site.
any_vmethods(const match_t<DexMethod, P>& p /* = any<DexMethod>() */) {
  return {[](const DexClass* cls, const match_t<DexMethod, P>& p) {
            for (const auto& vmethod : cls->get_vmethods()) {
              if (p.matches(vmethod)) return true;
            }
            return false;
          },
          p};
}

/** Match classes satisfying the given method match all vmethods */
template <typename P>
match_t<DexClass, std::tuple<match_t<DexMethod, P>>> all_vmethods(
    const match_t<DexMethod, P>& p) {
  return {[](const DexClass* cls, const match_t<DexMethod, P>& p) {
            for (const auto& vmethod : cls->get_vmethods()) {
              if (!p.matches(vmethod)) return false;
            }
            return true;
          },
          p};
}

/** Match classes satisfying the given method match for at most n vmethods */
template <typename P>
match_t<DexClass, std::tuple<size_t, match_t<DexMethod, P>>> at_most_n_vmethods(
    size_t n, const match_t<DexMethod, P>& p) {
  return {
      [](const DexClass* cls, const size_t& n, const match_t<DexMethod, P>& p) {
        size_t c = 0;
        for (const auto& vmethod : cls->get_vmethods()) {
          if (p.matches(vmethod)) c++;
          if (c > n) return false;
        }
        return true;
      },
      n, p};
}

/** Match classes satisfying the given method match for exactly n vmethods */
template <typename P>
match_t<DexClass, std::tuple<size_t, match_t<DexMethod, P>>> exactly_n_vmethods(
    size_t n, const match_t<DexMethod, P>& p) {
  return {
      [](const DexClass* cls, const size_t& n, const match_t<DexMethod, P>& p) {
        size_t c = 0;
        for (const auto& vmethod : cls->get_vmethods()) {
          if (p.matches(vmethod)) c++;
        }
        return c == n;
      },
      n, p};
}

/** Match classes satisfying the given method match for at least n vmethods */
template <typename P>
match_t<DexClass, std::tuple<size_t, match_t<DexMethod, P>>>
at_least_n_vmethods(size_t n, const match_t<DexMethod, P>& p) {
  return {
      [](const DexClass* cls, const size_t& n, const match_t<DexMethod, P>& p) {
        size_t c = 0;
        for (const auto& vmethod : cls->get_vmethods()) {
          if (p.matches(vmethod)) c++;
          if (c >= n) return true;
        }
        return false;
      },
      n, p};
}

/** Match classes satisfying the given method match for any dmethods */
template <typename P>
match_t<DexClass, std::tuple<match_t<DexMethod, P>>> any_dmethods(
    const match_t<DexMethod, P>& p) {
  return {[](const DexClass* cls, const match_t<DexMethod, P>& p) {
            for (const auto& dmethod : cls->get_dmethods()) {
              if (p.matches(dmethod)) return true;
            }
            return false;
          },
          p};
}

/** Match classes satisfying the given method match for all dmethods */
template <typename P>
match_t<DexClass, std::tuple<match_t<DexMethod, P>>> all_dmethods(
    const match_t<DexMethod, P>& p) {
  return {[](const DexClass* cls, const match_t<DexMethod, P>& p) {
            for (const auto& dmethod : cls->get_dmethods()) {
              if (!p.matches(dmethod)) return false;
            }
            return true;
          },
          p};
}

/** Match classes satisfying the given method match for at most n dmethods */
template <typename P>
match_t<DexClass, std::tuple<size_t, match_t<DexMethod, P>>> at_most_n_dmethods(
    size_t n, const match_t<DexMethod, P>& p) {
  return {
      [](const DexClass* cls, const size_t& n, const match_t<DexMethod, P>& p) {
        size_t c = 0;
        for (const auto& dmethod : cls->get_dmethods()) {
          if (p.matches(dmethod)) c++;
          if (c > n) return false;
        }
        return true;
      },
      n, p};
}

/** Match classes satisfying the given method match for exactly n dmethods */
template <typename P>
match_t<DexClass, std::tuple<size_t, match_t<DexMethod, P>>> exactly_n_dmethods(
    size_t n, const match_t<DexMethod, P>& p) {
  return {
      [](const DexClass* cls, const size_t& n, const match_t<DexMethod, P>& p) {
        size_t c = 0;
        for (const auto& dmethod : cls->get_dmethods()) {
          if (p.matches(dmethod)) c++;
        }
        return c == n;
      },
      n, p};
}

/** Match classes satisfying the given method match for at least n dmethods */
template <typename P>
match_t<DexClass, std::tuple<size_t, match_t<DexMethod, P>>>
at_least_n_dmethods(size_t n, const match_t<DexMethod, P>& p) {
  return {
      [](const DexClass* cls, const size_t& n, const match_t<DexMethod, P>& p) {
        size_t c = 0;
        for (const auto& dmethod : cls->get_dmethods()) {
          if (p.matches(dmethod)) c++;
          if (c >= n) return true;
        }
        return false;
      },
      n, p};
}

/** Match classes satisfying the given field match for any ifields */
template <typename P>
match_t<DexClass, std::tuple<match_t<DexField, P>>> any_ifields(
    const match_t<DexField, P>& p) {
  return {[](const DexClass* cls, const match_t<DexField, P>& p) {
            for (const auto& ifield : cls->get_ifields()) {
              if (p.matches(ifield)) return true;
            }
            return false;
          },
          p};
}

/** Match classes satisfying the given field match for any sfields */
template <typename P>
match_t<DexClass, std::tuple<match_t<DexField, P>>> any_sfields(
    const match_t<DexField, P>& p) {
  return {[](const DexClass* cls, const match_t<DexField, P>& p) {
            for (const auto& sfield : cls->get_sfields()) {
              if (p.matches(sfield)) return true;
            }
            return false;
          },
          p};
}

/** Match dex members containing any annotation that matches the given match */
template <typename T, typename P>
match_t<T, std::tuple<match_t<DexAnnotation, P>>> any_annos(
    const match_t<DexAnnotation, P>& predicate) {
  return {[](const T* t, const match_t<DexAnnotation, P>& p) {
            if (!t->is_def()) return false;
            const auto& anno_set = t->get_anno_set();
            if (!anno_set) return false;
            for (const auto& anno : anno_set->get_annotations()) {
              if (p.matches(anno)) {
                return true;
              }
            }
            return false;
          },
          predicate};
}

/**
 * Match which checks for membership of T in container C via C::find(T)
 * Does not take ownership of the container.
 */
template <typename T, typename C>
match_t<T, std::tuple<const C*>> in(const C* c) {
  return {[](const T* t, const C* const& c) {
            return c->find(const_cast<T*>(t)) != c->end();
          },
          c};
}

/**
 * Maps match<T, X> => match<DexType(t), X>
 */
template <typename T, typename P>
match_t<T, std::tuple<match_t<DexType, P>>> as_type(
    const match_t<DexType, P>& p) {
  return {[](const T* t, const match_t<DexType, P>& p) {
            return p.matches(t->type());
          },
          p};
}

/**
 * Maps match<DexType, X> => match<DexClass, X>
 */
template <typename T, typename P>
match_t<T, std::tuple<match_t<DexClass, P>>> as_class(
    const match_t<DexClass, P>& p) {
  return {[](const T* t, const match_t<DexClass, P>& p) {
            auto cls = type_class(t);
            return cls && p.matches(cls);
          },
          p};
}

/** Match which checks can_delete helper for DexMembers */
template <typename T>
match_t<T, std::tuple<>> can_delete() {
  return {[](const T* t) { return can_delete(t); }};
}

/** Match which checks can_rename helper for DexMembers */
template <typename T>
match_t<T, std::tuple<>> can_rename() {
  return {[](const T* t) { return can_rename(t); }};
}

/** Match which checks keep helper for DexMembers */
template <typename T>
match_t<T, std::tuple<>> has_keep() {
  return {[](const T* t) { return has_keep(t); }};
}

} // namespace m
