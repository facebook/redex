/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <exception>

#include <boost/exception/all.hpp>

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

/*
 * An assert-like macro that throws an exception.
 */

#define RUNTIME_CHECK(C, E)     \
  if (!(C)) {                   \
    BOOST_THROW_EXCEPTION((E)); \
  }
