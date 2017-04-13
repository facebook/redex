/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <boost/graph/adjacency_list.hpp>
#include <boost/range/iterator_range.hpp>

#include "DexClass.h"

typedef uint16_t Register;

struct RegisterValue {

  explicit RegisterValue(Register r) : kind(Kind::REGISTER), reg(r) {}
  explicit RegisterValue(int64_t l) : kind(Kind::CONST_LITERAL), literal(l) {}
  explicit RegisterValue(DexString* s) : kind(Kind::CONST_STRING), str(s) {}
  explicit RegisterValue(DexType* t) : kind(Kind::CONST_TYPE), type(t) {}
  explicit RegisterValue() : kind(Kind::NONE), dummy() {}

  bool operator==(const RegisterValue& other) const {
    if (kind != other.kind) {
      return false;
    }

    switch (kind) {
    case Kind::REGISTER:
      return reg == other.reg;
    case Kind::CONST_LITERAL:
      return literal == other.literal;
    case Kind::CONST_STRING:
      return str == other.str;
    case Kind::CONST_TYPE:
      return type == other.type;
    case Kind::NONE:
      return true;
    default:
      always_assert_log(false, "unknown RegisterValue kind");
    }
  }

  bool operator!=(const RegisterValue& other) const {
    return !(*this == other);
  }

  static const RegisterValue& none() {
    static const RegisterValue s_none;
    return s_none;
  }

 private:
  const enum class Kind {
    REGISTER,
    CONST_LITERAL,
    CONST_STRING,
    CONST_TYPE,
    NONE,
  } kind;

  const union {
    Register const reg;
    int64_t const literal;
    DexString* const str;
    DexType* const type;
    std::nullptr_t const dummy;
  };
};

class AliasedRegisters {
 public:
  AliasedRegisters() {}

  /**
   * declare that r1 and r2 are aliases of each other.
   * This also means r1 is aliased to all of r2's aliases and vice versa.
   */
  void make_aliased(RegisterValue r1, RegisterValue r2);

  /**
   * break every alias that any register has to `r`
   */
  void break_alias(RegisterValue r);

  /**
   * Including transitive aliases
   */
  bool are_aliases(RegisterValue r1, RegisterValue r2);

 private:
  // an undirected graph where register values are vertices
  // and an edge means they are aliased
  typedef boost::adjacency_list<boost::vecS,
                                boost::vecS,
                                boost::undirectedS,
                                RegisterValue>
      Graph;
  typedef boost::graph_traits<Graph>::vertex_descriptor vertex_t;
  Graph m_graph;

  const boost::range_detail::integer_iterator<vertex_t> find(RegisterValue r);
  vertex_t find_or_create(RegisterValue r);

  bool path_exists(vertex_t v1, vertex_t v2) const;
};
