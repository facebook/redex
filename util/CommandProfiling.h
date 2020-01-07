/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/optional.hpp>
#include <string>

class ScopedCommandProfiling final {
 public:
  explicit ScopedCommandProfiling(
      boost::optional<std::string> cmd,
      boost::optional<std::string> post_cmd = boost::none);

  ~ScopedCommandProfiling();

 private:
  pid_t m_profiler{-1};
  boost::optional<std::string> m_post_cmd;
};
