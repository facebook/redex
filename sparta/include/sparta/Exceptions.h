/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <exception>

#include <boost/config.hpp>
#include <boost/current_function.hpp>
#include <boost/exception/all.hpp>

namespace sparta {

/*
 * The base class of all exceptions in the abstract interpretation library.
 */
class abstract_interpretation_exception : public virtual std::exception,
                                          public virtual boost::exception {};

/*
 * This exception flags an inconsistent internal state.
 */
class internal_error : public virtual abstract_interpretation_exception {};

/*
 * This exception flags the argument to an operation that holds an invalid value
 * in the given context.
 */
class invalid_argument : public virtual abstract_interpretation_exception {};

/*
 * This exception flags the use of an operation outside its domain of
 * definition.
 */
class undefined_operation : public virtual abstract_interpretation_exception {};

/*
 * The attributes of an exception.
 */
using error_msg = boost::error_info<struct tag_error_msg, std::string>;

using argument_name = boost::error_info<struct tag_argument_name, std::string>;

using operation_name =
    boost::error_info<struct tag_operation_name, std::string>;

} // namespace sparta

/*
 * Like BOOST_THROW_EXCEPTION, but makes any path reaching the throw cold.
 *
 * Construction of the exception itself is quite expensive, so it is hoisted
 * into a lambda and lazily called if needed.
 */
#define SPARTA_THROW_EXCEPTION(E) \
  SPARTA_THROW_EXCEPTION_IMPL(E, __FILE__, __LINE__)

/*
 * An assert-like macro that throws an exception.
 */
#define SPARTA_RUNTIME_CHECK(C, E) \
  do                               \
    if (BOOST_UNLIKELY(!(C))) {    \
      SPARTA_THROW_EXCEPTION((E)); \
    }                              \
  while (0)

/*
 * Indicates that the given variable is unused, to prevent compiler warnings.
 */
#define SPARTA_UNUSED_VARIABLE(v) static_cast<void>(v)

namespace sparta::exception_impl {

template <class E>
BOOST_NORETURN void throw_exception(const E& x,
                                    const char* current_function,
                                    const char* file,
                                    int line) {
  // Emulates BOOST_THROW_EXCEPTION directly, except with the given context.
#if !defined(BOOST_EXCEPTION_DISABLE)
  boost::throw_exception(boost::enable_error_info(x)
                         << boost::throw_function(current_function)
                         << boost::throw_file(file) << boost::throw_line(line));
#else
  ::boost::ignore_unused(current_function);
  ::boost::ignore_unused(file);
  ::boost::ignore_unused(line);
  ::boost::throw_exception(x);
#endif
}

} // namespace sparta::exception_impl

/*
 * Marks a function as cold; the compiler won't optimize for the calling path.
 */
#ifdef __GNUC__
#define SPARTA_COLD_FUNCTION __attribute__((cold))
#else
#define SPARTA_COLD_FUNCTION
#endif

/*
 * We could just mark the lambda as the cold call, but this results in a huge
 * semantic-less symbol for the function. It is difficult to make sense of
 * `<giant anonymous lambda name>::operator()()` while debugging/profiling.
 * Instead, a local struct containing the line number wraps this call into a
 * legible (and actually useful) name.
 *
 * Care is taken to ensure this call requires no arguments, except those used
 * for constructing the exception (i.e., captured by the lambda). Since the
 * reported throwing function will change as a result of this lambda, we store
 * it ourselves as constexpr to avoid capturing. (Whether the current function
 * is given as a macro expansion or as a magic-defined static variable is up to
 * the compiler, hence why we can't treat it similar to __FILE__ and __LINE__).
 *
 * Double macro expansion is needed to concat with the line number.
 */
#define SPARTA_THROW_EXCEPTION_IMPL(E, File, Line) \
  SPARTA_THROW_EXCEPTION_IMPL2(E, File, Line)
#define SPARTA_THROW_EXCEPTION_IMPL2(E, File, Line)             \
  do {                                                          \
    constexpr auto& kCurrentFunction = BOOST_CURRENT_FUNCTION;  \
    auto throw_exception_impl = [&]() BOOST_NORETURN {          \
      ::sparta::exception_impl::throw_exception(                \
          (E), kCurrentFunction, File, Line);                   \
    };                                                          \
    struct L##Line {                                            \
      decltype(throw_exception_impl) impl;                      \
      BOOST_NORETURN BOOST_NOINLINE SPARTA_COLD_FUNCTION void   \
      throw_exception() {                                       \
        impl();                                                 \
      }                                                         \
    };                                                          \
    L##Line{std::move(throw_exception_impl)}.throw_exception(); \
  } while (0)
