/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

enum RedexError {
  // define errors here
  INTERNAL_ERROR = 1,
  GENERIC_ASSERTION_ERROR = 2,
  CACHE_INDEX_OUT_OF_BOUND = 3,
  DUPLICATE_CLASSES = 4,
  DUPLICATE_METHODS = 5,
};

class RedexException : public std::exception {
 public:
  const RedexError type;
  const std::string message;
  const std::map<std::string, std::string> extra_info;

  RedexException(RedexError type_of_error,
                 const std::string& message = "",
                 const std::map<std::string, std::string>& extra_info = {});

  virtual const char* what() const throw();

 private:
  std::string m_msg;
};

void assert_or_throw(bool cond,
                     RedexError type = RedexError::GENERIC_ASSERTION_ERROR,
                     const std::string& message = "",
                     const std::map<std::string, std::string>& extra_info = {});
