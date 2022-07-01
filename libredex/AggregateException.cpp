/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AggregateException.h"

#include <iostream>

void run_rethrow_first_aggregate(const std::function<void()>& f) {
  try {
    f();
  } catch (const aggregate_exception& ae) {
    if (ae.m_exceptions.size() > 1) {
      // We cannot modify exceptions. Log the other messages to stderr.
      std::cerr << "Too many exceptions. Other exceptions: " << std::endl;
      for (auto it = ae.m_exceptions.begin() + 1; it != ae.m_exceptions.end();
           ++it) {
        try {
          std::rethrow_exception(*it);
        } catch (const std::exception& e) {
          std::cerr << " " << e.what() << std::endl;
        } catch (...) {
          std::cerr << " (Not a std::exception)" << std::endl;
        }
      }
    }
    // Rethrow the first one.
    std::rethrow_exception(ae.m_exceptions.at(0));
  }
}
