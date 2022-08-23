/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/optional.hpp>
#include <string>

class ScopedCommandProfiling final {
 public:
  struct ProfilerInfo {
    std::string command;
    boost::optional<std::string> shutdown_cmd;
    boost::optional<std::string> post_cmd;
    ProfilerInfo(const std::string& command,
                 const boost::optional<std::string>& shutdown_cmd,
                 const boost::optional<std::string>& post_cmd)
        : command(command), shutdown_cmd(shutdown_cmd), post_cmd(post_cmd) {}
  };

  explicit ScopedCommandProfiling(
      const std::string& cmd,
      boost::optional<std::string> shutdown_cmd = boost::none,
      boost::optional<std::string> post_cmd = boost::none,
      // Note: using a string* to simplify API and help avoid copies.
      const char* log_str = nullptr);
  explicit ScopedCommandProfiling(
      const std::string& cmd,
      boost::optional<std::string> shutdown_cmd = boost::none,
      boost::optional<std::string> post_cmd = boost::none,
      const std::string* log_str = nullptr)
      : ScopedCommandProfiling(cmd,
                               std::move(shutdown_cmd),
                               std::move(post_cmd),
                               log_str == nullptr ? nullptr
                                                  : log_str->c_str()) {}
  template <typename T> // Allowed variants: std::string, char
  explicit ScopedCommandProfiling(const ProfilerInfo& info,
                                  const T* log_str = nullptr)
      : ScopedCommandProfiling(
            info.command, info.shutdown_cmd, info.post_cmd, log_str) {}
  ScopedCommandProfiling(const ScopedCommandProfiling&) = delete;
  ScopedCommandProfiling(ScopedCommandProfiling&&) noexcept;

  ~ScopedCommandProfiling();

  ScopedCommandProfiling& operator=(const ScopedCommandProfiling&) = delete;
  ScopedCommandProfiling& operator=(ScopedCommandProfiling&&) noexcept;

  static boost::optional<ProfilerInfo> maybe_info_from_env(
      const std::string& prefix);
  template <typename T>
  static boost::optional<ScopedCommandProfiling> maybe_from_info(
      const boost::optional<ProfilerInfo>& info, const T* log_str = nullptr);
  template <typename T>
  static boost::optional<ScopedCommandProfiling> maybe_from_env(
      const std::string& prefix, const T* log_str = nullptr) {
    return maybe_from_info(maybe_info_from_env(prefix), log_str);
  }

 private:
  int m_profiler{-1};
  // Run this shutdown command to end the profiling, instead of SIGINT
  boost::optional<std::string> m_shutdown_cmd;
  // After the profiling process has finished, run this command.
  boost::optional<std::string> m_post_cmd;
};
