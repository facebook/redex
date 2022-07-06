/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <exception>
#include <functional>
#include <vector>

// One or more exceptions
class aggregate_exception : public std::exception {
 public:
  template <class T>
  explicit aggregate_exception(T container)
      : m_exceptions(container.begin(), container.end()) {}

  // We do not really want to have this called directly
  const char* what() const throw() override { return "one or more exception"; }

  const std::vector<std::exception_ptr> m_exceptions;
};

void run_rethrow_first_aggregate(const std::function<void()>& f);
