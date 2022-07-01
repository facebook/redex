/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <bitset>
#include <cstdint>
#include <initializer_list>
#include <iosfwd>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/container/flat_map.hpp>
#include <boost/optional.hpp>

#include "ControlFlow.h"
#include "DexClass.h"
#include "PointsToSemanticsUtils.h"
#include "S_Expression.h"
#include "TypeSystem.h"

/*
 * The points-to semantics of Dex code defined here can be used for performing
 * flow-insensitive, inclusion-based points-to analyses. Each method is
 * translated into a system of points-to equations operating on sets of abstract
 * object instances. The actual representation of abstract object instances is
 * delegated to the particular points-to analysis that ultimately processes
 * these equations. This representation may vary depending on the type of
 * abstraction implemented in the points-to analysis (context-sensitivity,
 * object-sensitivity, etc.). The points-to equations abstract away all
 * computational aspects that are not directly related to pointer manipulation
 * (like scalar values and arithmetic operations) and is oblivious of
 * control-flow dependencies (a sequence of statements is interpreted as all
 * possible interleavings of the statements).
 *
 * Example:
 *
 * Consider the following class:
 *
 *   public class MyList {
 *     MyList next;
 *     Element val;
 *
 *     public MyList(Element v, MyList n) {
 *       next = n;
 *       val = v;
 *     }
 *
 *     public Element nth(int n) {
 *       MyList x = this;
 *       for(int i = 0; i < n; ++i) {
 *         x = x.next;
 *       }
 *       return x.val;
 *     }
 *   }
 *
 * The points-to equations of the method `nth` computed by the semantic
 * translator are as follows:
 *
 *   LMyList;#nth: (I)LElement; {
 *     V0 = THIS
 *     V4 = V0 + V1
 *     V1 = V4.LMyList;#next
 *     V2 = V4.LElement;#val
 *     RETURN V2
 *   }
 *
 * Note that all arithmetic computations have been abstracted away. The
 * variables are not necessarily numbered sequentially, as irrelevant equations
 * may have been optimized away. The list traversal is encoded by the recursive
 * definition of variable V4 (where `U` denotes the union of sets of abstract
 * object instances). The meaning of this recursive definition is given by the
 * least solution of the system of set equations (or equivalently, by the least
 * fixpoint of the associated set transformer). The literature on points-to
 * analysis is vast, but the fundamental concepts of inclusion-based,
 * flow-insensitive analysis go back to Andersen's PhD thesis, which is also an
 * excellent introduction to the topic:
 *
 *   L. Andersen. Program Analysis and Specialization for the C Programming
 *   Language. PhD thesis, DIKU, University of Copenhagen, 1994.
 *
 * The points-to equations don't keep any connection with the original Dex code
 * aside from references to type and method names. This is intentional, as we
 * want to be able to modify the points-to equations (for better accuracy and/or
 * scalability) without having to worry about consistency with the original
 * code.
 */

// Forward declaration.
class PointsToSemantics;

/*
 * A points-to variable denotes a set of abstract object instances. It is
 * uniquely identified by a positive number.
 */
class PointsToVariable final {
 public:
  // The default constructor produces the `null` variable to prevent confusion
  // with user-defined variables.
  PointsToVariable() : m_id(null_var_id()) {}

  // This variable has a special meaning: it represents the empty set of
  // abstract object instances, i.e., the semantic interpretation of `null`.
  static PointsToVariable null_variable() {
    return PointsToVariable(null_var_id());
  }

  // This variable represents the special parameter `this` in instance methods.
  static PointsToVariable this_variable() {
    return PointsToVariable(this_var_id());
  }

  sparta::s_expr to_s_expr() const;

  static boost::optional<PointsToVariable> from_s_expr(const sparta::s_expr& e);

 private:
  static constexpr int32_t null_var_id() { return -1; }

  static constexpr int32_t this_var_id() { return -2; }

  explicit PointsToVariable(size_t id) : m_id(id) {}

  // A user-defined variable always has a positive identifier. We use negative
  // identifiers for special variables, like the `null` variable.
  int32_t m_id;

  friend class PointsToMethodSemantics;
  friend size_t hash_value(const PointsToVariable&);
  friend bool operator==(const PointsToVariable&, const PointsToVariable&);
  friend bool operator<(const PointsToVariable&, const PointsToVariable&);
  friend std::ostream& operator<<(std::ostream&, const PointsToVariable&);
};

/*
 * We want to use points-to variables in various STL containers with minimal
 * effort, hence the definition of the following functions.
 */

// To enable the use of boost::hash.
size_t hash_value(const PointsToVariable& v);

bool operator==(const PointsToVariable& v, const PointsToVariable& w);

bool operator<(const PointsToVariable& v, const PointsToVariable& w);

std::ostream& operator<<(std::ostream& o, const PointsToVariable& v);

/*
 * Except for the disjunction, points-to operations are similar to their
 * counterparts in Dex bytecode. Note that we do not attempt to model exceptions
 * precisely. The `PTS_GET_EXCEPTION` operation stands for `move-exception`, but
 * assumes that any exception can be caught. This also explains why we have no
 * operation corresponding to `throw`. As for the disjuction, it's simply the
 * union of points-to variables (V = V1 U V2 U ... U Vn). We also introduce a
 * special operation `PTS_GET_CLASS` for java.lang.Object#getClass(), since
 * java.lang.Class objects need to be handled specially by the analyzer.
 */

// clang-format off
//                              is_load  is_get  is_put  is_invoke
#define PTS_OPS \
   PTS_OP(PTS_CONST_STRING,     true ,   false,  false,  false    ) \
I  PTS_OP(PTS_CONST_CLASS,      true ,   false,  false,  false    ) \
I  PTS_OP(PTS_GET_EXCEPTION,    true ,   false,  false,  false    ) \
I  PTS_OP(PTS_NEW_OBJECT,       true ,   false,  false,  false    ) \
I  PTS_OP(PTS_LOAD_PARAM,       true ,   false,  false,  false    ) \
I  PTS_OP(PTS_GET_CLASS,        false,   false,  false,  false    ) \
I  PTS_OP(PTS_CHECK_CAST,       false,   false,  false,  false    ) \
I  PTS_OP(PTS_IGET,             false,   true ,  false,  false    ) \
I  PTS_OP(PTS_IGET_SPECIAL,     false,   true ,  false,  false    ) \
I  PTS_OP(PTS_SGET,             false,   true ,  false,  false    ) \
I  PTS_OP(PTS_IPUT,             false,   false,  true ,  false    ) \
I  PTS_OP(PTS_IPUT_SPECIAL,     false,   false,  true ,  false    ) \
I  PTS_OP(PTS_SPUT,             false,   false,  true ,  false    ) \
I  PTS_OP(PTS_INVOKE_VIRTUAL,   false,   false,  false,  true     ) \
I  PTS_OP(PTS_INVOKE_SUPER,     false,   false,  false,  true     ) \
I  PTS_OP(PTS_INVOKE_DIRECT,    false,   false,  false,  true     ) \
I  PTS_OP(PTS_INVOKE_INTERFACE, false,   false,  false,  true     ) \
I  PTS_OP(PTS_INVOKE_STATIC,    false,   false,  false,  true     ) \
I  PTS_OP(PTS_RETURN,           false,   false,  false,  false    ) \
I  PTS_OP(PTS_DISJUNCTION,      false,   false,  false,  false    )
// clang-format on

#define MAX_PTS_OPS (sizeof(unsigned long long) * 8)

#define PTS_OP(OP, ...) OP
#define I ,
enum PointsToOperationKind { PTS_OPS };
#undef I
#undef PTS_OP

/*
 * We need a special edge to model the points-to relation between an array and
 * its elements. In the future, we could also use special edges to model the
 * effect of external libraries or native methods.
 */
enum SpecialPointsToEdge {
  PTS_ARRAY_ELEMENT,
};

struct PointsToOperation {
  PointsToOperationKind kind;
  union {
    DexMethodRef* dex_method;
    DexFieldRef* dex_field;
    const DexString* dex_string;
    DexType* dex_type;
    size_t parameter;
    SpecialPointsToEdge special_edge;
  };

  PointsToOperation() = default;

  explicit PointsToOperation(PointsToOperationKind k) : kind(k) {}

  PointsToOperation(PointsToOperationKind k, DexMethodRef* m)
      : kind(k), dex_method(m) {}

  PointsToOperation(PointsToOperationKind k, DexFieldRef* f)
      : kind(k), dex_field(f) {}

  PointsToOperation(PointsToOperationKind k, const DexString* s)
      : kind(k), dex_string(s) {}

  PointsToOperation(PointsToOperationKind k, DexType* t)
      : kind(k), dex_type(t) {}

  PointsToOperation(PointsToOperationKind k, size_t p)
      : kind(k), parameter(p) {}

  PointsToOperation(PointsToOperationKind k, SpecialPointsToEdge e)
      : kind(k), special_edge(e) {}

  bool is_load() const {
#define PTS_OP(OP, is_load, is_get, is_put, is_invoke) \
  ((is_load) ? (1ULL << (OP)) : 0)
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define I |
    static const std::bitset<MAX_PTS_OPS> load_operations(PTS_OPS);
#undef I
#undef PTS_OP
    return load_operations[kind];
  }

  bool is_get_class() const { return kind == PTS_GET_CLASS; }

  bool is_check_cast() const { return kind == PTS_CHECK_CAST; }

  bool is_get() const {
#define PTS_OP(OP, is_load, is_get, is_put, is_invoke) \
  ((is_get) ? (1ULL << (OP)) : 0)
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define I |
    static const std::bitset<MAX_PTS_OPS> get_operations(PTS_OPS);
#undef I
#undef PTS_OP
    return get_operations[kind];
  }

  bool is_sget() const { return kind == PTS_SGET; }

  bool is_put() const {
#define PTS_OP(OP, is_load, is_get, is_put, is_invoke) \
  ((is_put) ? (1ULL << (OP)) : 0)
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define I |
    static const std::bitset<MAX_PTS_OPS> put_operations(PTS_OPS);
#undef I
#undef PTS_OP
    return put_operations[kind];
  }

  bool is_sput() const { return kind == PTS_SPUT; }

  bool is_invoke() const {
#define PTS_OP(OP, is_load, is_get, is_put, is_invoke) \
  ((is_invoke) ? (1ULL << (OP)) : 0)
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define I |
    static const std::bitset<MAX_PTS_OPS> invoke_operations(PTS_OPS);
#undef I
#undef PTS_OP
    return invoke_operations[kind];
  }

  bool is_virtual_call() const { return is_invoke() && !is_static_call(); }

  bool is_static_call() const { return kind == PTS_INVOKE_STATIC; }

  bool is_return() const { return kind == PTS_RETURN; }

  bool is_disjunction() const { return kind == PTS_DISJUNCTION; }

  sparta::s_expr to_s_expr() const;

  static boost::optional<PointsToOperation> from_s_expr(
      const sparta::s_expr& e);
};

/*
 * We don't use the term points-to equation here, because strictly speaking,
 * some operations are not equational (like a method call with no return value).
 */
class PointsToAction final {
 public:
  PointsToAction() = default;

  const PointsToOperation& operation() const { return m_operation; }

  bool has_dest() const { return m_arguments.count(dest_key()) > 0; }

  PointsToVariable lhs() const { return get_arg(lhs_key()); }

  PointsToVariable rhs() const { return get_arg(rhs_key()); }

  PointsToVariable instance() const { return get_arg(instance_key()); }

  PointsToVariable dest() const { return get_arg(dest_key()); }

  PointsToVariable src() const { return get_arg(src_key()); }

  void remove_dest() { m_arguments.erase(dest_key()); }

  /*
   * Returns the arguments of a method call (indexed by their position in the
   * invocation) or the components of a disjunction.
   */
  std::vector<std::pair<size_t, PointsToVariable>> get_arguments() const;

  /*
   * Used to build PTS_CONST_STRING, PTS_CONST_CLASS, PTS_GET_EXCEPTION,
   * PTS_NEW_OBJECT and LOAD_PARAM actions.
   */
  static PointsToAction load_operation(const PointsToOperation& operation,
                                       PointsToVariable dest);

  /*
   * Used to build a PTS_GET_CLASS action.
   */
  static PointsToAction get_class_operation(PointsToVariable dest,
                                            PointsToVariable src);
  /*
   * Used to build a PTS_CHECK_CAST action.
   */
  static PointsToAction check_cast_operation(DexType* dex_type,
                                             PointsToVariable dest,
                                             PointsToVariable src);

  /*
   * Used to build PTS_IGET, PTS_IGET_SPECIAL and PTS_SGET actions. There is no
   * instance for PTS_SGET.
   */
  static PointsToAction get_operation(
      const PointsToOperation& operation,
      PointsToVariable dest,
      boost::optional<PointsToVariable> instance = {});

  /*
   * Used to build PTS_IPUT, PTS_IPUT_SPECIAL and PTS_SPUT actions. There is no
   * lhs for PTS_SPUT.
   */
  static PointsToAction put_operation(
      const PointsToOperation& operation,
      PointsToVariable rhs,
      boost::optional<PointsToVariable> lhs = {});

  /*
   * Used to build PTS_INVOKE_VIRTUAL, PTS_INVOKE_SUPER, PTS_INVOKE_DIRECT,
   * PTS_INVOKE_INTERFACE and PTS_INVOKE_STATIC actions. There is no instance
   * for PTS_INVOKE_STATIC. The optional dest parameter is used to model the
   * return value of the method call if any. The arguments of the method call
   * are numbered starting from 0.
   */
  static PointsToAction invoke_operation(
      const PointsToOperation& operation,
      boost::optional<PointsToVariable> dest,
      boost::optional<PointsToVariable> instance,
      const std::vector<std::pair<int32_t, PointsToVariable>>& args);

  /*
   * Used to build a PTS_RETURN action.
   */
  static PointsToAction return_operation(PointsToVariable src);

  /*
   * Used to build a disjunction of variables v = v1 + ... + vn.
   */
  template <typename InputIterator>
  static PointsToAction disjunction(PointsToVariable dest,
                                    InputIterator first,
                                    InputIterator last);

  sparta::s_expr to_s_expr() const;

  static boost::optional<PointsToAction> from_s_expr(const sparta::s_expr& e);

 private:
  static constexpr int32_t lhs_key() { return -1; }
  static constexpr int32_t rhs_key() { return -2; }
  static constexpr int32_t instance_key() { return -3; }
  static constexpr int32_t dest_key() { return -4; }
  static constexpr int32_t src_key() { return -5; }

  PointsToAction(
      const PointsToOperation& operation,
      std::initializer_list<std::pair<int32_t, PointsToVariable>> arguments)
      : m_operation(operation), m_arguments(arguments) {
    m_arguments.shrink_to_fit();
  }

  PointsToAction(
      const PointsToOperation& operation,
      const std::vector<std::pair<int32_t, PointsToVariable>>& arguments);

  PointsToVariable get_arg(int32_t key) const;

  PointsToOperation m_operation;
  // We use a flat map (i.e., a sorted associative array) to get the convenience
  // of a map while maintaining a low memory footprint. The arguments in a
  // method call are denoted by positive indexes that correspond to their
  // position in the original invocation. Arguments specific to a points-to
  // operation (like the left-hand side of an assignment operation) have a
  // negative index.
  boost::container::flat_map<int32_t, PointsToVariable> m_arguments;
};

std::ostream& operator<<(std::ostream& o, const PointsToAction& a);

/*
 * During the analysis we may want to distinguish among methods that don't have
 * points-to equations because either the code is unavailable (external
 * libraries, native methods), the code doesn't exist (abstract methods) or the
 * code exists but has no effect on pointers. Each case may be subject to a
 * different semantic intepretation.
 */
enum MethodKind {
  PTS_APK, // Regular method defined in the APK
  PTS_ABSTRACT, // Abstract method
  PTS_NATIVE, // Native method
  PTS_STUB, // The set of points-to equations for the method is a stub
};

/*
 * The system of points-to actions representing the semantics of a method,
 * together with some context information.
 */
class PointsToMethodSemantics {
 public:
  PointsToMethodSemantics(DexMethodRef* dex_method,
                          MethodKind kind,
                          size_t start_var_id,
                          size_t size_hint);

  DexMethodRef* get_method() const { return m_dex_method; }

  MethodKind kind() const { return m_kind; }

  PointsToVariable get_new_variable() {
    return PointsToVariable(m_variable_counter++);
  }

  const std::vector<PointsToAction>& get_points_to_actions() const {
    return m_points_to_actions;
  }

  void add(const PointsToAction& a) { m_points_to_actions.emplace_back(a); }

  /*
   * This function attempts to remove points-to equations that have no effect on
   * the analysis (e.g., reading a value that is not used in any write operation
   * or method call). This helps relieve some of the computational burden on
   * the resolution algorithm.
   */
  void shrink();

  sparta::s_expr to_s_expr() const;

  static boost::optional<PointsToMethodSemantics> from_s_expr(
      const sparta::s_expr& e);

 private:
  DexMethodRef* m_dex_method;
  MethodKind m_kind;
  // The variable counter allows us to generate new variables when we need to
  // modify the system of points-to actions (e.g., for inlining method calls).
  size_t m_variable_counter;
  std::vector<PointsToAction> m_points_to_actions;

  friend std::ostream& operator<<(std::ostream&,
                                  const PointsToMethodSemantics&);
};

std::ostream& operator<<(std::ostream& o, const PointsToMethodSemantics& s);

/*
 * This represents the points-to semantics of all methods inside a given scope.
 *
 * IMPORTANT: the procedure used to generate the points-to sematics assumes that
 * invoke-* instructions are in denormalized form, i.e., wide arguments are
 * explicitly represented by a pair of consecutive registers. The generation of
 * the points-to semantics doesn't modify the IR and hence, can be used anywhere
 * in Redex.
 */
class PointsToSemantics final {
 public:
  using iterator = std::unordered_map<DexMethodRef*,
                                      PointsToMethodSemantics>::const_iterator;

  PointsToSemantics() = delete;

  PointsToSemantics(const PointsToSemantics& other) = delete;

  PointsToSemantics& operator=(const PointsToSemantics& other) = delete;

  /*
   * The constructor generates points-to actions for all methods in the given
   * scope. The generation is performed in parallel using a pool of threads. If
   * the flag `generate_stubs` is set to true, all methods in the scope are
   * interpreted as stubs.
   */
  explicit PointsToSemantics(const Scope& scope, bool generate_stubs = false);

  /*
   * The stubs are stored in the specified text file as S-expressions. In case
   * of a collision between a method in the APK and a stub, the stub is
   * discarded.
   */
  void load_stubs(const std::string& file_name);

  iterator begin() { return m_method_semantics.begin(); }

  iterator end() { return m_method_semantics.end(); }

  const TypeSystem& get_type_system() { return m_type_system; }

  boost::optional<PointsToMethodSemantics*> get_method_semantics(
      DexMethodRef* dex_method);

 private:
  MethodKind default_method_kind() const;

  void initialize_entry(DexMethod* dex_method);

  void generate_points_to_actions(DexMethod* dex_method);

  bool m_generate_stubs;
  TypeSystem m_type_system;
  PointsToSemanticsUtils m_utils;
  std::unordered_map<DexMethodRef*, PointsToMethodSemantics> m_method_semantics;

  friend std::ostream& operator<<(std::ostream&, const PointsToSemantics&);
};

std::ostream& operator<<(std::ostream& o, const PointsToSemantics& s);
