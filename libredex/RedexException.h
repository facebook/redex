/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
  // Error codes here may also be referenced from py runner scripts for
  // supplimental/custom error messages.
  INTERNAL_ERROR = 1,
  GENERIC_ASSERTION_ERROR = 2,
  CACHE_INDEX_OUT_OF_BOUND = 3,
  DUPLICATE_CLASSES = 4,
  DUPLICATE_METHODS = 5,
  BAD_ANNOTATION = 6,
  UNSATISFIED_ANALYSIS_PASS = 7,
  REJECTED_CODING_PATTERN = 8,
  INVALID_BETAMAP = 9,
  BUFFER_END_EXCEEDED = 10,
  TYPE_CHECK_ERROR = 11,
  INVALID_DEX = 12,
  INVALID_JAVA = 13,
  MAX = 13,
};

class RedexException : public std::exception {
 public:
  const RedexError type;
  const std::string message;
  const std::map<std::string, std::string> extra_info;

  explicit RedexException(
      RedexError type_of_error,
      const std::string& message = "",
      const std::map<std::string, std::string>& extra_info = {});

  const char* what() const noexcept override;

 private:
  std::string m_msg;
};

namespace redex {

class InvalidDexException : public RedexException {
 public:
  explicit InvalidDexException(
      const std::string& message,
      const std::map<std::string, std::string>& extra_info = {})
      : RedexException(RedexError::INVALID_DEX, message, extra_info) {}
};

class BufferEndExceededException : public RedexException {
 public:
  explicit BufferEndExceededException(
      const std::string& message,
      const std::map<std::string, std::string>& extra_info = {})
      : RedexException(RedexError::BUFFER_END_EXCEEDED, message, extra_info) {}
};

class DuplicateMethodsException : public RedexException {
 public:
  explicit DuplicateMethodsException(
      const std::string& message,
      const std::map<std::string, std::string>& extra_info = {})
      : RedexException(RedexError::DUPLICATE_METHODS, message, extra_info) {}
};

class BadAnnotationException : public RedexException {
 public:
  explicit BadAnnotationException(
      const std::string& message,
      const std::map<std::string, std::string>& extra_info = {})
      : RedexException(RedexError::BAD_ANNOTATION, message, extra_info) {}
};

class InvalidJavaException : public RedexException {
 public:
  explicit InvalidJavaException(
      const std::string& message,
      const std::map<std::string, std::string>& extra_info = {})
      : RedexException(RedexError::INVALID_JAVA, message, extra_info) {}
};

} // namespace redex

void assert_or_throw(bool cond,
                     RedexError type = RedexError::GENERIC_ASSERTION_ERROR,
                     const std::string& message = "",
                     const std::map<std::string, std::string>& extra_info = {});
