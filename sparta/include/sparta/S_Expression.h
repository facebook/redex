/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <istream>
#include <iterator>
#include <memory>
#include <ostream>
#include <sstream>
#include <stack>
#include <string>
#include <vector>

#include <boost/functional/hash.hpp>
#include <boost/functional/hash_fwd.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>

#include <sparta/Exceptions.h>

namespace sparta {

// Forward declarations.
namespace s_expr_impl {

class Component;
class Pattern;

} // namespace s_expr_impl

/*
 * S-expressions are the syntactic structure of the Lisp family of languages.
 * They're also a compact, efficient and human-readable format for serializing
 * complex data structures (see this document for a more detailed rationale:
 * http://people.csail.mit.edu/rivest/Sexp.txt). In particular, the artifacts
 * created when performing abstract interpretation (semantic equations, program
 * invariants) can be conveniently represented using S-expressions.
 *
 * This library implements a simple form of S-expressions that is sufficient for
 * most serialization purposes. There are two types of atoms: strings and 32-bit
 * signed integers. In the serialized form, integers are prefixed with the `#`
 * sign and strings are quoted (escape sequences are permitted inside strings).
 * By analogy with the symbols of Lisp, if a string contains only alphanumeric
 * characters and underscores, the double quotes are optional. Lists of elements
 * are represented using parentheses. Note that in our representation and unlike
 * Lisp, the empty list `()`, also commonly called nil, is not an atom.
 *
 * Examples of S-expressions:
 *   #12
 *   "a string\n"
 *   (a (b c) d) ; a comment
 *   ((#-1 "a, b, c") (#0 d) (#1 ()))
 *
 * Note that a number that is not prefixed with the `#` character is interpreted
 * as a string.
 *
 * S-expressions are immutable, functional data structures that can share
 * subcomponents. In the following code for example, the S-expression e1 is not
 * duplicated in e2 and e3, but shared instead:
 *
 *   auto e1 = s_expr({s_expr("a"), s_expr("b")});
 *   auto e2 = s_expr({s_expr("l1"), e1});
 *   auto e3 = s_expr({s_expr("l2"), e1});
 *
 * The disposal of S-expressions is managed by shared pointers under the hood.
 */
class s_expr final {
 public:
  /*
   * The default constructor returns the empty list `()`.
   */
  s_expr();

  /*
   * Constructs an 32-bit signed integer atom.
   */
  explicit s_expr(int32_t n);

  /*
   * Constructs a string atom.
   */
  explicit s_expr(const char* s);

  /*
   * Constructs a string atom.
   */
  explicit s_expr(std::string_view s);

  /*
   * Constructs a string atom.
   */
  explicit s_expr(std::string s);

  /*
   * Various constructors for a list. The empty list (nil) can be constructed
   * with `s_expr({})`.
   */
  explicit s_expr(std::initializer_list<s_expr> l);

  explicit s_expr(const std::vector<s_expr>& l);

  template <typename InputIterator>
  s_expr(InputIterator first, InputIterator last);

  /*
   * Predicate checking whether the S-expression is the empty list `()` or not.
   */
  bool is_nil() const;

  /*
   * Note that nil (the empty list) is not an atom.
   */
  bool is_atom() const;

  bool is_int32() const;

  bool is_string() const;

  bool is_list() const;

  /*
   * Returns the value of an integer atom.
   */
  int32_t get_int32() const;

  /*
   * Returns the contents of a string atom.
   */
  const std::string& get_string() const;

  /*
   * Returns the size of a list.
   */
  size_t size() const;

  /*
   * Returns the element of a list at the given position. The first element of a
   * list has index 0.
   */
  s_expr operator[](size_t index) const;

  /*
   * This operation returns the given list minus its first n elements. For
   * example, if e is the S-expression `(a (b) (c d) e)`, then:
   *   e.tail(2) = ((c d) e)
   *   e.tail(4) = ()
   */
  s_expr tail(size_t n) const;

  /*
   * Structural equality test of S-expressions. If both expressions share some
   * subcomponents, the complexity of this operation can be sublinear.
   */
  bool equals(const s_expr& other) const;

  /*
   * The complexity of computing the hash value is linear in the size of the
   * S-expression.
   */
  size_t hash_value() const;

  /*
   * Outputs a standard representation of the S-expression on the given stream.
   */
  void print(std::ostream& output) const;

  /*
   * Returns a standard representation of the S-expression as a string.
   */
  std::string str() const;

  friend bool operator==(const s_expr& e1, const s_expr& e2) {
    return e1.equals(e2);
  }

  friend bool operator!=(const s_expr& e1, const s_expr& e2) {
    return !e1.equals(e2);
  }

  /*
   * We need this function in order to use boost::hash<s_expr>.
   */
  friend size_t hash_value(const s_expr& e) { return e.hash_value(); }

 private:
  // Appends an element to a list. This operation is used during parsing.
  void add_element(s_expr element);

  // By construction, m_component can never be null.
  boost::intrusive_ptr<s_expr_impl::Component> m_component;

  friend class s_expr_istream;
};

} // namespace sparta

inline std::ostream& operator<<(std::ostream& output, const sparta::s_expr& e) {
  e.print(output);
  return output;
}

namespace sparta {

/*
 * This is a stream-like structure for parsing S-expressions from a character
 * stream input.
 *
 * Example usage:
 *   std::string str = "(a b) (c);";
 *   std::istringstream input(str);
 *   s_expr_istream si(input);
 *   s_expr e1, e2, e3;
 *   si >> e1 >> e2;
 *   // e1 = (a b) and e2 = (c)
 *   si >> e3;
 *   // parse error: si.fail() returns true
 *   // si.what() contains the error message
 */
class s_expr_istream final {
 public:
  s_expr_istream() = delete;

  s_expr_istream(const s_expr_istream&) = delete;

  s_expr_istream& operator=(const s_expr_istream&) = delete;

  s_expr_istream(std::istream& input)
      : m_input(input), m_line_number(1), m_status(Status::Good) {}

  s_expr_istream& operator>>(s_expr& expr);

  /*
   * These status functions are inspired from the standard stream API.
   */

  bool good() const { return m_status == Status::Good; }

  bool fail() const { return m_status != Status::Good; }

  /*
   * When failing to read an S-expression from the input stream, we want to
   * distinguish between reaching the end of the input and an actual parse
   * error.
   */
  bool eoi() const { return m_status == Status::EOI; }

  /*
   * When failing to read an S-expression from the input stream, we can use this
   * function to obtain a description of the error.
   */
  const std::string& what() const;

 private:
  enum class Status { EOI, Good, Fail };

  void skip_white_spaces();

  void set_status_eoi();
  void set_status_fail(std::string what_arg);

  std::stack<s_expr> m_stack;
  std::istream& m_input;
  size_t m_line_number;
  Status m_status;
  mutable bool m_what_expanded{false};
  mutable std::unique_ptr<std::string> m_what;
};

/*
 * S-expressions are primarily intended to be used as a serialization format for
 * complex data structures. When deserializing an S-expression, it would be very
 * cumbersome to inspect its structure using the basic accessor functions only.
 * Pattern matching is a more compact and readable way of filtering
 * S-expressions according to structural criteria. For example, imagine we have
 * an encoding of functions as S-expressions:
 *
 *   (function (name "my function") (package "my package")
 *             (args (#1 "arg1") (#2 "arg2") (#3 "arg3")))
 *
 * We want to extract the name and arguments of a function while ignoring the
 * package name. Using the pattern-matching mechanism for S-expressions, this
 * can be coded as follows:
 *
 *   std::string name;
 *   s_expr args;
 *   if(s_patn({s_patn("function"),
 *              s_patn({s_patn("name"), s_patn(&name)}),
 *              s_patn({s_patn("package"), s_patn()}),
 *              s_patn({s_patn("args")}, args)}).match_with(e)) {
 *     // name = "my function"
 *     // args = ((#1 "arg1") (#2 "arg2") (#3 "arg3"))
 *     ...
 *   }
 */
class s_patn {
 public:
  class pattern_matching_error
      : public virtual abstract_interpretation_exception {};

  /*
   * The wildcard pattern matches any S-expression.
   */
  s_patn();

  /*
   * Matches any S-expression and stores it into the given placeholder.
   */
  explicit s_patn(s_expr& placeholder);

  /*
   * Matches an integer atom with the given value.
   */
  explicit s_patn(int32_t n);

  /*
   * Matches any integer atom and stores its value into the given placeholder.
   */
  explicit s_patn(int32_t* placeholder);

  /*
   * Matches a string atom with the given value.
   */
  explicit s_patn(const char* s);

  /*
   * Matches a string atom with the given value.
   */
  explicit s_patn(std::string_view s);

  /*
   * Matches a string atom with the given value.
   */
  explicit s_patn(std::string s);

  /*
   * Matches any string atom and stores its value into the given placeholder.
   */
  explicit s_patn(std::string* placeholder);

  /*
   * Matches any string atom and stores a pointer to its value into the given
   * placeholder pointer.
   */
  explicit s_patn(const std::string** placeholder_ptr);

  /*
   * Matches each element in a list with the given sequence of patterns.
   */
  explicit s_patn(std::initializer_list<s_patn> heads);

  /*
   * Matches the first elements of a list with the given sequence of patterns
   * and stores the remaining elements into the given placeholder. If there are
   * no remaining elements, tail contains the empty list upon successful
   * matching.
   *
   * Example:
   *   Assume e = ((1 2) 3 4 (5 6))
   *
   *   s_expr x, y, t;
   *   if (s_patn({s_patn(x), s_patn(y)}, t).match_with(e)) {
   *     // x = (1 2)
   *     // y = 3
   *     // t = (4 (5 6))
   *     ...
   *   }
   */
  s_patn(std::initializer_list<s_patn> heads, s_expr& tail);

  /*
   * Returns true if the pattern matches the given S-expression. Upon successful
   * matching, all placeholders in the pattern are be set to their corresponding
   * values. If the matching fails, the values of the placeholders are
   * undefined.
   */
  bool match_with(const s_expr& expr);

  void must_match(const s_expr& expr, std::string_view msg);

 private:
  // By construction, m_pattern can never be null.
  boost::intrusive_ptr<s_expr_impl::Pattern> m_pattern;
};

namespace s_expr_impl {

enum class ComponentKind { Int32Atom, StringAtom, List };

// Checks whether a character belongs to a Lisp-like symbol, i.e., a string atom
// that can be represented without quotes.
inline bool is_symbol_char(char c) {
  return std::isalnum(c) || c == '_' || c == '-' || c == '/' || c == ':' ||
         c == '.';
}

using IntrusiveSharedComponent = boost::intrusive_ptr<Component>;
class Component {
 public:
  virtual ~Component() {}

  explicit Component(ComponentKind kind) : m_kind(kind) {}

  ComponentKind kind() const { return m_kind; }

  virtual bool equals(const IntrusiveSharedComponent& other) const = 0;

  virtual size_t hash_value() const = 0;

  virtual void print(std::ostream& o) const = 0;

  friend void intrusive_ptr_add_ref(const Component* p) {
    p->m_reference_count++;
  }

  friend void intrusive_ptr_release(const Component* p) {
    if (p->m_reference_count-- == 1) {
      delete p;
    }
  }

 protected:
  mutable size_t m_reference_count{0};
  ComponentKind m_kind;
};

class Int32Atom final : public Component {
 public:
  explicit Int32Atom(int32_t n)
      : Component(ComponentKind::Int32Atom), m_value(n) {}

  int32_t get_value() const { return m_value; }

  bool equals(const IntrusiveSharedComponent& other) const {
    if (this == other.get()) {
      // Since S-expressions can share structure, checking for pointer equality
      // prevents unnecessary computations.
      return true;
    }
    if (other->kind() != ComponentKind::Int32Atom) {
      return false;
    }
    auto n = boost::static_pointer_cast<Int32Atom>(other);
    return m_value == n->m_value;
  }

  size_t hash_value() const {
    boost::hash<int32_t> hasher;
    return hasher(m_value);
  }

  void print(std::ostream& output) const { output << "#" << m_value; }

 private:
  int32_t m_value;
};

class StringAtom final : public Component {
 public:
  explicit StringAtom(std::string s)
      : Component(ComponentKind::StringAtom), m_string(std::move(s)) {}

  const std::string& get_string() const { return m_string; }

  bool equals(const IntrusiveSharedComponent& other) const {
    if (this == other.get()) {
      // Since S-expressions can share structure, checking for pointer equality
      // prevents unnecessary computations.
      return true;
    }
    if (other->kind() != ComponentKind::StringAtom) {
      return false;
    }
    auto s = boost::static_pointer_cast<StringAtom>(other);
    return m_string == s->m_string;
  }

  size_t hash_value() const {
    boost::hash<std::string> hasher;
    return hasher(m_string);
  }

  void print(std::ostream& output) const {
    if (m_string.empty()) {
      // The empty string needs to be explicitly represented.
      output << "\"\"";
      return;
    }
    if (std::find_if(m_string.begin(), m_string.end(), [](char c) {
          return !is_symbol_char(c);
        }) == m_string.end()) {
      // If the string only contains alphanumeric characters and underscores, we
      // display it without quotes.
      output << m_string;
    } else {
      // The string is quoted and special characters are displayed using escape
      // sequences.
      output << std::quoted(m_string);
    }
  }

 private:
  std::string m_string;
};

class List final : public Component {
 public:
  List() : Component(ComponentKind::List), m_list(0) { m_list.shrink_to_fit(); }

  template <typename InputIterator>
  List(InputIterator first, InputIterator last)
      : Component(ComponentKind::List), m_list(first, last) {
    m_list.shrink_to_fit();
  }

  size_t size() const { return m_list.size(); }

  s_expr get_element(size_t index) const {
    if (index < m_list.size()) {
      return m_list[index];
    } else {
      SPARTA_THROW_EXCEPTION(invalid_argument() << argument_name("index"));
    }
  }

  s_expr tail(size_t index) const {
    SPARTA_RUNTIME_CHECK(index <= m_list.size(),
                         invalid_argument() << argument_name("index"));
    // If index == m_list.size(), the function returns the empty list.
    return s_expr(std::next(m_list.begin(), index), m_list.end());
  }

  void add_element(s_expr element) { m_list.push_back(std::move(element)); }

  bool equals(const IntrusiveSharedComponent& other) const {
    if (this == other.get()) {
      // Since S-expressions can share structure, checking for pointer equality
      // prevents unnecessary computations.
      return true;
    }
    if (other->kind() != ComponentKind::List) {
      return false;
    }
    auto l = boost::static_pointer_cast<List>(other);
    if (l->m_list.size() != m_list.size()) {
      return false;
    }
    return std::equal(
        m_list.begin(),
        m_list.end(),
        l->m_list.begin(),
        [](const s_expr& e1, const s_expr& e2) { return e1.equals(e2); });
  }

  size_t hash_value() const {
    return boost::hash_range(m_list.begin(), m_list.end());
  }

  void print(std::ostream& output) const {
    output << "(";
    for (auto it = m_list.begin(); it != m_list.end(); ++it) {
      it->print(output);
      if (std::next(it) != m_list.end()) {
        output << " ";
      }
    }
    output << ")";
  }

 private:
  std::vector<s_expr> m_list;
};

using IntrusiveSharedPattern = boost::intrusive_ptr<Pattern>;
class Pattern {
 public:
  virtual ~Pattern() {}

  virtual bool match_with(const s_expr& expr) = 0;

  friend void intrusive_ptr_add_ref(const Pattern* p) {
    p->m_reference_count++;
  }

  friend void intrusive_ptr_release(const Pattern* p) {
    if (p->m_reference_count-- == 1) {
      delete p;
    }
  }

 protected:
  mutable size_t m_reference_count{0};
};

class WildcardPattern final : public Pattern {
 public:
  WildcardPattern() = default;

  bool match_with(const s_expr& expr) { return true; }
};

class PlaceholderPattern final : public Pattern {
 public:
  explicit PlaceholderPattern(s_expr& placeholder)
      : m_placeholder(placeholder) {}

  bool match_with(const s_expr& expr) {
    m_placeholder = expr;
    return true;
  }

 private:
  s_expr& m_placeholder;
};

class Int32Pattern final : public Pattern {
 public:
  explicit Int32Pattern(int32_t n) : m_value(n) {}

  explicit Int32Pattern(int32_t* placeholder) : m_placeholder(placeholder) {}

  bool match_with(const s_expr& expr) {
    if (!expr.is_int32()) {
      return false;
    }
    if (m_placeholder) {
      **m_placeholder = expr.get_int32();
      return true;
    }
    return m_value == expr.get_int32();
  }

 private:
  int32_t m_value;
  boost::optional<int32_t*> m_placeholder;
};

class StringPattern final : public Pattern {
 public:
  explicit StringPattern(std::string s) : m_string(std::move(s)) {}

  explicit StringPattern(std::string* placeholder)
      : m_placeholder(placeholder) {}

  bool match_with(const s_expr& expr) {
    if (!expr.is_string()) {
      return false;
    }
    if (m_placeholder) {
      **m_placeholder = expr.get_string();
      return true;
    }
    return m_string == expr.get_string();
  }

 private:
  std::string m_string;
  boost::optional<std::string*> m_placeholder;
};

class StringPattern2 final : public Pattern {
 public:
  explicit StringPattern2(const std::string** placeholder_ptr)
      : m_placeholder_ptr(placeholder_ptr) {}

  bool match_with(const s_expr& expr) {
    if (!expr.is_string()) {
      return false;
    }
    *m_placeholder_ptr = &expr.get_string();
    return true;
  }

 private:
  const std::string** m_placeholder_ptr;
};

class ListPattern final : public Pattern {
 public:
  explicit ListPattern(std::initializer_list<s_patn> heads) : m_heads(heads) {}

  ListPattern(std::initializer_list<s_patn> heads, s_expr& tail)
      : m_heads(heads), m_tail(tail) {}

  bool match_with(const s_expr& expr) {
    if (!expr.is_list()) {
      return false;
    }
    if (m_heads.size() > expr.size()) {
      return false;
    }
    for (size_t i = 0; i < m_heads.size(); ++i) {
      if (!m_heads[i].match_with(expr[i])) {
        return false;
      }
    }
    if (m_tail) {
      m_tail->get() = expr.tail(m_heads.size());
    }
    return true;
  }

 private:
  std::vector<s_patn> m_heads;
  boost::optional<std::reference_wrapper<s_expr>> m_tail;
};

} // namespace s_expr_impl

inline s_expr::s_expr() : m_component(new s_expr_impl::List()) {}

inline s_expr::s_expr(int32_t n) : m_component(new s_expr_impl::Int32Atom(n)) {}

inline s_expr::s_expr(const char* s)
    : m_component(new s_expr_impl::StringAtom(std::string(s))) {}

inline s_expr::s_expr(std::string_view s)
    : m_component(new s_expr_impl::StringAtom(std::string(s))) {}

inline s_expr::s_expr(std::string s)
    : m_component(new s_expr_impl::StringAtom(std::move(s))) {}

inline s_expr::s_expr(std::initializer_list<s_expr> l)
    : m_component(new s_expr_impl::List(l.begin(), l.end())) {}

inline s_expr::s_expr(const std::vector<s_expr>& l)
    : m_component(new s_expr_impl::List(l.begin(), l.end())) {}

template <typename InputIterator>
inline s_expr::s_expr(InputIterator first, InputIterator last)
    : m_component(new s_expr_impl::List(first, last)) {}

inline bool s_expr::is_nil() const { return is_list() && (size() == 0); }

inline bool s_expr::is_atom() const { return !is_list(); }

inline bool s_expr::is_int32() const {
  return m_component->kind() == s_expr_impl::ComponentKind::Int32Atom;
}

inline bool s_expr::is_string() const {
  return m_component->kind() == s_expr_impl::ComponentKind::StringAtom;
}

inline bool s_expr::is_list() const {
  return m_component->kind() == s_expr_impl::ComponentKind::List;
}

inline int32_t s_expr::get_int32() const {
  SPARTA_RUNTIME_CHECK(is_int32(), undefined_operation());
  return boost::static_pointer_cast<s_expr_impl::Int32Atom>(m_component)
      ->get_value();
}

inline const std::string& s_expr::get_string() const {
  SPARTA_RUNTIME_CHECK(is_string(), undefined_operation());
  return boost::static_pointer_cast<s_expr_impl::StringAtom>(m_component)
      ->get_string();
}

inline size_t s_expr::size() const {
  SPARTA_RUNTIME_CHECK(is_list(), undefined_operation());
  return boost::static_pointer_cast<s_expr_impl::List>(m_component)->size();
}

inline s_expr s_expr::operator[](size_t index) const {
  SPARTA_RUNTIME_CHECK(is_list(), undefined_operation());
  return boost::static_pointer_cast<s_expr_impl::List>(m_component)
      ->get_element(index);
}

inline s_expr s_expr::tail(size_t index) const {
  SPARTA_RUNTIME_CHECK(is_list(), undefined_operation());
  return boost::static_pointer_cast<s_expr_impl::List>(m_component)
      ->tail(index);
}

inline bool s_expr::equals(const s_expr& other) const {
  if (m_component == other.m_component) {
    // Since S-expressions can share structure, checking for pointer equality
    // prevents unnecessary computations.
    return true;
  }
  return m_component->equals(other.m_component);
}

inline size_t s_expr::hash_value() const { return m_component->hash_value(); }

inline void s_expr::print(std::ostream& output) const {
  m_component->print(output);
}

inline std::string s_expr::str() const {
  std::ostringstream out;
  print(out);
  return out.str();
}

inline void s_expr::add_element(s_expr element) {
  SPARTA_RUNTIME_CHECK(m_component->kind() == s_expr_impl::ComponentKind::List,
                       invalid_argument()
                           << argument_name("element")
                           << operation_name("s_expr::add_element()"));
  auto list = boost::static_pointer_cast<s_expr_impl::List>(m_component);
  list->add_element(std::move(element));
}

inline s_expr_istream& s_expr_istream::operator>>(s_expr& expr) {
  std::string s;
  for (;;) {
    skip_white_spaces();
    if (!m_input.good()) {
      if (!m_stack.empty()) {
        set_status_fail("Incomplete S-expression");
      } else {
        set_status_eoi();
      }
      return *this;
    }
    char next_char = m_input.peek();
    switch (next_char) {
    case '(': {
      m_stack.emplace();
      m_input.get();
      break;
    }
    case ')': {
      if (m_stack.empty()) {
        set_status_fail("Extra ')' encountered");
        return *this;
      }
      m_input.get();
      s_expr list = std::move(m_stack.top());
      m_stack.pop();
      if (m_stack.empty()) {
        expr = std::move(list);
        return *this;
      }
      m_stack.top().add_element(std::move(list));
      break;
    }
    case '#': {
      m_input.get();
      int32_t n;
      m_input >> n;
      if (m_input.fail()) {
        set_status_fail("Error parsing int32_t literal");
        return *this;
      }
      if (m_stack.empty()) {
        expr = s_expr(n);
        return *this;
      }
      m_stack.top().add_element(s_expr(n));
      break;
    }
    case '"': {
      m_input >> std::quoted(s);
      if (m_input.fail()) {
        set_status_fail("Error parsing string literal");
        return *this;
      }
      if (m_stack.empty()) {
        expr = s_expr(std::move(s));
        return *this;
      }
      m_stack.top().add_element(s_expr(std::move(s)));
      break;
    }
    case ';': {
      m_input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
      ++m_line_number;
      break;
    }
    default: {
      // The next S-expression is necessary a symbol, i.e., an unquoted string.
      if (!s_expr_impl::is_symbol_char(next_char)) {
        std::ostringstream out;
        out << "Unexpected character encountered: '" << next_char << "'";
        set_status_fail(out.str());
        return *this;
      }
      std::string symbol;
      while (m_input.good() && s_expr_impl::is_symbol_char(next_char)) {
        symbol.append({next_char});
        m_input.get();
        next_char = m_input.peek();
      }
      symbol.shrink_to_fit();
      if (m_stack.empty()) {
        expr = s_expr(std::move(symbol));
        return *this;
      }
      m_stack.top().add_element(s_expr(std::move(symbol)));
    }
    }
  }
}

inline void s_expr_istream::skip_white_spaces() {
  for (;;) {
    char c = m_input.peek();
    if (m_input.good() && std::isspace(c)) {
      if (c == '\n') {
        ++m_line_number;
      }
      m_input.get();
    } else {
      return;
    }
  }
}

inline void s_expr_istream::set_status_eoi() {
  m_status = Status::EOI;
  m_what = nullptr;
  m_what_expanded = false;
}

inline void s_expr_istream::set_status_fail(std::string what_arg) {
  m_status = Status::Fail;
  m_what = std::make_unique<std::string>(std::move(what_arg));
  m_what_expanded = false;
}

inline const std::string& s_expr_istream::what() const {
  if (m_what_expanded) {
    return *m_what;
  }
  std::ostringstream ss;
  ss << "On line " << m_line_number << ": ";
  switch (m_status) {
  case Status::EOI:
    ss << "End of input";
    break;
  case Status::Good:
    ss << "OK";
    break;
  default:
    ss << *m_what;
    break;
  }
  m_what = std::make_unique<std::string>(ss.str());
  m_what_expanded = true;
  return *m_what;
}

inline s_patn::s_patn() : m_pattern(new s_expr_impl::WildcardPattern()) {}

inline s_patn::s_patn(s_expr& placeholder)
    : m_pattern(new s_expr_impl::PlaceholderPattern(placeholder)) {}

inline s_patn::s_patn(int32_t n)
    : m_pattern(new s_expr_impl::Int32Pattern(n)) {}

inline s_patn::s_patn(int32_t* placeholder)
    : m_pattern(new s_expr_impl::Int32Pattern(placeholder)) {}

inline s_patn::s_patn(const char* s)
    : m_pattern(new s_expr_impl::StringPattern(std::string(s))) {}

inline s_patn::s_patn(std::string_view s)
    : m_pattern(new s_expr_impl::StringPattern(std::string(s))) {}

inline s_patn::s_patn(std::string s)
    : m_pattern(new s_expr_impl::StringPattern(std::move(s))) {}

inline s_patn::s_patn(std::string* placeholder)
    : m_pattern(new s_expr_impl::StringPattern(placeholder)) {}

inline s_patn::s_patn(const std::string** placeholder_ptr)
    : m_pattern(new s_expr_impl::StringPattern2(placeholder_ptr)) {}

inline s_patn::s_patn(std::initializer_list<s_patn> heads)
    : m_pattern(new s_expr_impl::ListPattern(heads)) {}

inline s_patn::s_patn(std::initializer_list<s_patn> heads, s_expr& tail)
    : m_pattern(new s_expr_impl::ListPattern(heads, tail)) {}

inline bool s_patn::match_with(const s_expr& expr) {
  return m_pattern->match_with(expr);
}

inline void s_patn::must_match(const s_expr& expr, std::string_view msg) {
  SPARTA_RUNTIME_CHECK(m_pattern->match_with(expr),
                       pattern_matching_error()
                           << error_msg("Could not find match against " +
                                        expr.str() + ": " + std::string(msg)));
}

} // namespace sparta
