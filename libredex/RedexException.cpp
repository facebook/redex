/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RedexException.h"
#include "Debug.h"
#include <stdarg.h>

RedexException::RedexException(
    RedexError type_of_error,
    const std::string& message,
    const std::map<std::string, std::string>& extra_info)
    : type(type_of_error), message(message), extra_info(extra_info) {

  std::ostringstream oss;
  if (type_of_error != RedexError::GENERIC_ASSERTION_ERROR) {
    oss << "RedexError: " << type << " with message: ";
  }
  oss << message;
  if (!extra_info.empty()) {
    oss << " with extra info:";
    for (auto it = extra_info.begin(); it != extra_info.end(); it++) {
      oss << " (\"" << it->first << "\", \"" << it->second << "\")";
    }
  }
  m_msg = oss.str();
}

const char* RedexException::what() const noexcept { return m_msg.c_str(); }

void assert_or_throw(bool cond,
                     RedexError type,
                     const std::string& message,
                     const std::map<std::string, std::string>& extra_info) {
  if (!cond) {
    if (redex::throw_typed_exception()) {
      switch (type) {
      case RedexError::DUPLICATE_METHODS:
        throw redex::DuplicateMethodsException(message, extra_info);
      case RedexError::BAD_ANNOTATION:
        throw redex::BadAnnotationException(message, extra_info);
      case RedexError::BUFFER_END_EXCEEDED:
        throw redex::BufferEndExceededException(message, extra_info);
      case RedexError::INVALID_DEX:
        throw redex::InvalidDexException(message, extra_info);
      case RedexError::INVALID_JAVA:
        throw redex::InvalidJavaException(message, extra_info);
      default:
        break;
      }
    }
    throw RedexException(type, message, extra_info);
  }
}
