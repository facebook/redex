/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>
#include <string>
#include <unistd.h>
#include <utility>

class ScopedCommandProfiling final {
 public:
  struct ProfilerInfo {
    std::string command;
    std::optional<std::string> shutdown_cmd;
    std::optional<std::string> post_cmd;
    ProfilerInfo(std::string command,
                 std::optional<std::string> shutdown_cmd,
                 std::optional<std::string> post_cmd)
        : command(std::move(command)),
          shutdown_cmd(std::move(shutdown_cmd)),
          post_cmd(std::move(post_cmd)) {}
  };

  explicit ScopedCommandProfiling(
      const std::string& cmd,
      std::optional<std::string> shutdown_cmd = std::nullopt,
      std::optional<std::string> post_cmd = std::nullopt,
      // Note: using a string* to simplify API and help avoid copies.
      const char* log_str = nullptr);
  explicit ScopedCommandProfiling(
      const std::string& cmd,
      std::optional<std::string> shutdown_cmd = std::nullopt,
      std::optional<std::string> post_cmd = std::nullopt,
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

  static std::optional<ProfilerInfo> maybe_info_from_env(
      const std::string& prefix);
  template <typename T>
  static std::optional<ScopedCommandProfiling> maybe_from_info(
      const std::optional<ProfilerInfo>& info, const T* log_str = nullptr);
  template <typename T>
  static std::optional<ScopedCommandProfiling> maybe_from_env(
      const std::string& prefix, const T* log_str = nullptr) {
    return maybe_from_info(maybe_info_from_env(prefix), log_str);
  }

 private:
  pid_t m_profiler{-1};
  // Run this shutdown command to end the profiling, instead of SIGINT
  std::optional<std::string> m_shutdown_cmd;
  // After the profiling process has finished, run this command.
  std::optional<std::string> m_post_cmd;
};
