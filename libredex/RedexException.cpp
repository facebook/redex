/**
 * Copyright (c) Facebook, Inc. and its affiliates.
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

const char* RedexException::what() const throw() { return m_msg.c_str(); }

void assert_or_throw(bool cond,
                     RedexError type,
                     const std::string& message,
                     const std::map<std::string, std::string>& extra_info) {
  if (!cond) {
    throw RedexException(type, message, extra_info);
  }
}
