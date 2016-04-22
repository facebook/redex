/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <algorithm>
#include <functional>
#include <vector>

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "ReachableClasses.h"

/**
 * Determine if the method is a constructore.
 *
 * Notes:
 * - Does NOT distinguish between <init> and <clinit>, will return true
 *   for static class initializers
 */
inline bool is_constructor(const DexMethod* meth) {
  return meth->get_access() & ACC_CONSTRUCTOR;
}

/** Determine if the method takes no arguments. */
inline bool has_no_args(const DexMethod* meth) {
  return meth->get_proto()->get_args()->get_type_list().empty();
}

/** Determine if the method takes exactly n arguments. */
inline bool has_n_args(const DexMethod* meth, size_t n) {
  return meth->get_proto()->get_args()->get_type_list().size() == n;
}

/**
 * Determine if the method has code.
 *
 * Notes:
 * - Native methods are not considered to "have code"
 */
inline bool has_code(const DexMethod* meth) {
  return meth->get_code();
}

/** Determine if the opcode matches any flavor of invoke-direct */
inline bool is_invoke_direct(const DexInstruction* insn) {
  auto op = insn->opcode();
  return op == OPCODE_INVOKE_DIRECT ||
    op == OPCODE_INVOKE_DIRECT_RANGE;
}

namespace m {

/** N-ary match template */
template <
  typename T,
  typename P = std::tuple<>,
  size_t N = std::tuple_size<P>::value >
struct match_t;

/** Nullary specialization of N-ary match */
template <typename T, typename P>
struct match_t<T, P, 0> {
  bool (*fn)(const T*);
  bool matches(const T* t) const {
    return fn(t);
  }
};

/** Unary specialization of N-ary match */
template <typename T, typename P>
struct match_t<T, P, 1> {
  using P0_t = typename std::tuple_element<0, P>::type;
  bool (*fn)(const T*, const P0_t& p0);
  P0_t p0;
  bool matches(const T* t) const {
    return fn(t, p0);
  }
};

/** Binary specialization of N-ary match */
template <typename T, typename P>
struct match_t<T, P, 2> {
  using P0_t = typename std::tuple_element<0, P>::type;
  using P1_t = typename std::tuple_element<1, P>::type;
  bool (*fn)(const T*, const P0_t& p0, const P1_t& p1);
  P0_t p0;
  P1_t p1;
  bool matches(const T* t) const {
    return fn(t, p0, p1);
  }
};

/** Match a subordinate match whose logical not is true */
template <typename T, typename P0>
match_t<T, std::tuple<match_t<T, P0> > >
  operator !(const match_t<T, P0>& p0) {
  return {
    [](const T* t, const match_t<T, P0>& p0) {
      return !p0.matches(t); },
    p0 };
}

/** Match two subordinate matches whose logical or is true */
template <typename T, typename P0, typename P1>
match_t<T, std::tuple<match_t<T, P0>, match_t<T, P1> > >
  operator ||(const match_t<T, P0>& p0, const match_t<T, P1>& p1) {
  return {
    [](const T* t, const match_t<T, P0>& p0, const match_t<T, P1>& p1) {
      return p0.matches(t) || p1.matches(t); },
    p0,
    p1 };
}

/** Match two subordinate matches whose logical and is true */
template <typename T, typename P0, typename P1>
match_t<T, std::tuple<match_t<T, P0>, match_t<T, P1> > >
  operator &&(const match_t<T, P0>& p0, const match_t<T, P1>& p1) {
  return {
    [](const T* t, const match_t<T, P0>& p0, const match_t<T, P1>& p1) {
      return p0.matches(t) && p1.matches(t); },
    p0,
    p1 };
}

/** Match two subordinate matches whose logical xor is true */
template <typename T, typename P0, typename P1>
match_t<T, std::tuple<match_t<T, P0>, match_t<T, P1> > >
  operator ^(const match_t<T, P0>& p0, const match_t<T, P1>& p1) {
  return {
    [](const T* t, const match_t<T, P0>& p0, const match_t<T, P1>& p1) {
      return p0.matches(t) ^ p1.matches(t); },
    p0,
    p1 };
}

/** Match any T (always matches) */
template<typename T>
match_t<T, std::tuple<> > any() {
  return {
    [](const T* t) {
      return true;
    }
  };
}

// N.B. free beer offer to anyone who can get 'named' to work on const char*
// instead of std::string&
/** Match any T named thusly */
template<typename T>
match_t<T, std::tuple<const std::string&> > named(const std::string& name) {
  return {
    [](const T* t, const std::string& name) {
      return !strcmp(t->get_name()->c_str(), name.c_str());
    },
    name
  };
}

/** Match T's which are external */
template<typename T>
match_t<T, std::tuple<> > is_external() {
  return {
    [](const T* t) {
      return t->is_external();
    }
  };
}

/** Match T's which are final */
template<typename T>
match_t<T, std::tuple<> > is_final() {
  return {
    [](const T* t) {
      return (bool)(t->get_access() & ACC_FINAL);
    }
  };
}

/** Match T's which are static */
template<typename T>
match_t<T, std::tuple<> > is_static() {
  return {
    [](const T* t) {
      return (bool)(t->get_access() & ACC_STATIC);
    }
  };
}

/** Match T's which are interfaces */
template<typename T>
match_t<T, std::tuple<> > is_abstract() {
  return {
    [](const T* t) {
      return (bool)(t->get_access() & ACC_ABSTRACT);
    }
  };
}

/** Match classes which are interfaces */
inline match_t<DexClass, std::tuple<> > is_interface() {
  return {
    [](const DexClass* cls) {
      return (bool)(cls->get_access() & ACC_INTERFACE);
    }
  };
}

struct DexOpcodeSeq {
public:
  const DexMethod* meth;
  std::vector<DexInstruction*>::const_iterator& it;
};

inline DexInstruction* next(const DexOpcodeSeq* opcs) {
  auto opcode = *(opcs->it);
  std::advance(opcs->it, 1);
  return opcode;
}

inline const DexInstruction* next(const DexInstruction* insn) {
  return insn;
}

// Some of these matchers can work with single opcodes, or
// sequences (iterators) of opcodes. Some can only work with sequences.

/**
 * Matches any class ref opcode, e.g. const-class
 */
template <typename T = DexOpcodeSeq>
match_t<T> has_types() {
  return {
    [](const T* opcodes) {
      auto opcode = next(opcodes);
      return opcode->has_types();
    }
  };
}

template <typename T = DexOpcodeSeq>
match_t<T> invoke_direct() {
  return {
    [](const T* opcodes) {
      auto opcode = next(opcodes)->opcode();
      return opcode == OPCODE_INVOKE_DIRECT ||
        opcode == OPCODE_INVOKE_DIRECT_RANGE;
    }
  };
}

template <typename T = DexOpcodeSeq>
match_t<T> invoke_static() {
  return {
    [](const T* opcodes) {
      auto opcode = next(opcodes)->opcode();
      return opcode == OPCODE_INVOKE_STATIC ||
        opcode == OPCODE_INVOKE_STATIC_RANGE;
    }
  };
}

template <typename T = DexOpcodeSeq>
match_t<T> return_void() {
  return {
    [](const T* opcodes) {
      auto opcode = next(opcodes)->opcode();
      return opcode == OPCODE_RETURN_VOID;
    }
  };
}

template<size_t N, typename T>
struct for_each_opcode_match {
  static bool visit(
    const T& t, const DexOpcodeSeq* opcodes) {
    // N.B. recursive template here to unwind our tuple of opcode matchers
    if (!for_each_opcode_match<N-1, T>::visit(t, opcodes)) return false;
    typename std::tuple_element<N-1, T>::type opc = std::get<N-1>(t);
    return opc.matches(opcodes);
  }
};

template<typename T>
struct for_each_opcode_match<0, T> {
  static bool visit(
    // N.B. base case of recursive template
    const T& t, const DexOpcodeSeq* opcodes) {
    return true;
  }
};

/** Match methods whose code satisfied the given opcodes match */
template <typename ...T>
match_t<DexMethod, std::tuple<std::tuple<T...> > >
  opcodes(const std::tuple<T...>& t) {
  return {
    [](const DexMethod* meth, const std::tuple<T...>& t) {
      auto code = meth->get_code();
      if (!code) return false;
      auto it = code->get_instructions().cbegin();
      DexOpcodeSeq opcodes = { meth, it };
      return for_each_opcode_match<
        std::tuple_size<std::tuple<T...> >::value,
        std::tuple<T...> >::visit(t, &opcodes);
    },
    t
  };
}

/** Match methods that are default constructors */
inline match_t<DexMethod, std::tuple<> > is_default_constructor() {
  return {
    [](const DexMethod* meth) {
      return !is_static(meth) &&
              is_constructor(meth) &&
              has_no_args(meth) &&
              has_code(meth) &&
              opcodes(std::make_tuple(
                invoke_direct(),
                return_void()
              )).matches(meth);
    }
  };
}

/** Match methods that are constructors. INCLUDES static constructors! */
inline match_t<DexMethod, std::tuple<> > is_constructor() {
  return {
    [](const DexMethod* meth) {
      return is_constructor(meth);
    }
  };
}

/** Match classes that are enums */
inline match_t<DexClass, std::tuple<> > is_enum() {
  return {
    [](const DexClass* cls) {
      return (bool)(cls->get_access() & ACC_ENUM);
    }
  };
}

/** Match classes satisfying the given method match for any vmethods */
template <typename P>
match_t<DexClass, std::tuple<match_t<DexMethod, P> > >
  // N.B. Free beer offer to anyone who can figure out how to get the
  // default argument below to work. It seems to somehow throw
  // off the inference of P at the call/template instantiation site.
  any_vmethods(const match_t<DexMethod, P>& p /* = any<DexMethod>() */) {
  return {
    [](const DexClass* cls, const match_t<DexMethod, P>& p) {
      for (const auto& vmethod : cls->get_vmethods()) {
        if (p.matches(vmethod)) return true;
      }
      return false;
    },
    p };
}

/** Match classes satisfying the given method match all vmethods */
template <typename P>
match_t<DexClass, std::tuple<match_t<DexMethod, P> > >
  all_vmethods(const match_t<DexMethod, P>& p) {
  return {
    [](const DexClass* cls, const match_t<DexMethod, P>& p) {
      for (const auto& vmethod : cls->get_vmethods()) {
        if (!p.matches(vmethod)) return false;
      }
      return true;
    },
    p };
}

/** Match classes satisfying the given method match for at most n vmethods */
template <typename P>
match_t<DexClass, std::tuple<size_t, match_t<DexMethod, P> > >
  at_most_n_vmethods(size_t n, const match_t<DexMethod, P>& p) {
  return {
    [](const DexClass* cls, const size_t& n, const match_t<DexMethod, P>& p) {
      size_t c = 0;
      for (const auto& vmethod : cls->get_vmethods()) {
        if (p.matches(vmethod)) c++;
        if (c > n) return false;
      }
      return true;
    },
    n,
    p };
}

/** Match classes satisfying the given method match for exactly n vmethods */
template <typename P>
match_t<DexClass, std::tuple<size_t, match_t<DexMethod, P> > >
  exactly_n_vmethods(size_t n, const match_t<DexMethod, P>& p) {
  return {
    [](const DexClass* cls, const size_t& n, const match_t<DexMethod, P>& p) {
      size_t c = 0;
      for (const auto& vmethod : cls->get_vmethods()) {
        if (p.matches(vmethod)) c++;
      }
      return c == n;
    },
    n,
    p };
}

/** Match classes satisfying the given method match for at least n vmethods */
template <typename P>
match_t<DexClass, std::tuple<size_t, match_t<DexMethod, P> > >
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
    n,
    p };
}

/** Match classes satisfying the given method match for any dmethods */
template <typename P>
match_t<DexClass, std::tuple<match_t<DexMethod, P> > >
  any_dmethods(const match_t<DexMethod, P>& p) {
  return {
    [](const DexClass* cls, const match_t<DexMethod, P>& p) {
      for (const auto& dmethod : cls->get_dmethods()) {
        if (p.matches(dmethod)) return true;
      }
      return false;
    },
    p };
}

/** Match classes satisfying the given method match for all dmethods */
template <typename P>
match_t<DexClass, std::tuple<match_t<DexMethod, P> > >
  all_dmethods(const match_t<DexMethod, P>& p) {
  return {
    [](const DexClass* cls, const match_t<DexMethod, P>& p) {
      for (const auto& dmethod : cls->get_dmethods()) {
        if (!p.matches(dmethod)) return false;
      }
      return true;
    },
    p };
}

/** Match classes satisfying the given method match for at most n dmethods */
template <typename P>
match_t<DexClass, std::tuple<size_t, match_t<DexMethod, P> > >
  at_most_n_dmethods(size_t n, const match_t<DexMethod, P>& p) {
  return {
    [](const DexClass* cls, const size_t& n, const match_t<DexMethod, P>& p) {
      size_t c = 0;
      for (const auto& dmethod : cls->get_dmethods()) {
        if (p.matches(dmethod)) c++;
        if (c > n) return false;
      }
      return true;
    },
    n,
    p };
}

/** Match classes satisfying the given method match for exactly n dmethods */
template <typename P>
match_t<DexClass, std::tuple<size_t, match_t<DexMethod, P> > >
  exactly_n_dmethods(size_t n, const match_t<DexMethod, P>& p) {
  return {
    [](const DexClass* cls, const size_t& n, const match_t<DexMethod, P>& p) {
      size_t c = 0;
      for (const auto& dmethod : cls->get_dmethods()) {
        if (p.matches(dmethod)) c++;
      }
      return c == n;
    },
    n,
    p };
}

/** Match classes satisfying the given method match for at least n dmethods */
template <typename P>
match_t<DexClass, std::tuple<size_t, match_t<DexMethod, P> > >
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
    n,
    p };
}

/** Match classes satisfying the given field match for any ifields */
template <typename P>
match_t<DexClass, std::tuple<match_t<DexField, P> > >
  any_ifields(const match_t<DexField, P>& p) {
  return {
    [](const DexClass* cls, const match_t<DexField, P>& p) {
      for (const auto& ifield : cls->get_ifields()) {
        if (p.matches(ifield)) return true;
      }
      return false;
    },
    p };
}

/** Match classes satisfying the given field match for any sfields */
template <typename P>
match_t<DexClass, std::tuple<match_t<DexField, P> > >
  any_sfields(const match_t<DexField, P>& p) {
  return {
    [](const DexClass* cls, const match_t<DexField, P>& p) {
      for (const auto& sfield : cls->get_sfields()) {
        if (p.matches(sfield)) return true;
      }
      return false;
    },
    p };
}

/** Match dex members containing any annotation that matches the given match */
template <typename T, typename P>
match_t<T, std::tuple<match_t<DexAnnotation, P> > >
  any_annos(const match_t<DexAnnotation, P>& p) {
  return {
    [](const T* t, const match_t<DexAnnotation, P>& p) {
      const auto& anno_set = t->get_anno_set();
      if (!anno_set) return false;
      for (const auto& anno : anno_set->get_annotations()) {
        if (p.matches(anno)) {
          return true;
        }
      }
      return false;
    },
    p };
}

/** Match which checks for membership of T in container C via C::find(T) */
template <typename T, typename C>
match_t<T, std::tuple<C> > in(const C& c) {
  return {
    [](const T* t, const C& c) {
      return c.find(const_cast<T*>(t)) != c.end(); },
    c };
}

/**
 * Maps match<T, X> => match<DexType(t), X>
 */
template <typename T, typename P>
match_t<T, std::tuple<match_t<DexType, P> > >
  as_type(const match_t<DexType, P>& p) {
  return {
    [](const T* t, const match_t<DexType, P>& p) {
      return p.matches(t->type());
    },
    p };
}

/** Match which checks can_delete helper for DexMembers */
template<typename T>
match_t<T, std::tuple<> > can_delete() {
  return {
    [](const T* t) {
      return can_delete(t);
    }
  };
}

/** Match which checks is_seed helper for DexMembers */
template<typename T>
match_t<T, std::tuple<> > is_seed() {
  return {
    [](const T* t) {
      return is_seed(t);
    }
  };
}

} // namespace m
