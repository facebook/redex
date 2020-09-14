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
      boost::optional<std::string> shutdown_cmd = boost::none,
      boost::optional<std::string> post_cmd = boost::none);
  ScopedCommandProfiling(const ScopedCommandProfiling&) = delete;
  ScopedCommandProfiling(ScopedCommandProfiling&&) noexcept;

  ~ScopedCommandProfiling();

  ScopedCommandProfiling& operator=(ScopedCommandProfiling&&) noexcept;

 private:
  pid_t m_profiler{-1};
  // Run this shutdown command to end the profiling, instead of SIGINT
  boost::optional<std::string> m_shutdown_cmd;
  // After the profiling process has finished, run this command.
  boost::optional<std::string> m_post_cmd;
};
