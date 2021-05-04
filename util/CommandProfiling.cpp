/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CommandProfiling.h"

#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

#include "Debug.h"

namespace {

pid_t spawn(const std::string& cmd) {
#ifdef _POSIX_VERSION
  auto child = fork();
  always_assert_log(child != -1, "Failed to fork");
  if (child != 0) {
    return child;
  } else {
    int ret = execl("/bin/sh", "/bin/sh", "-c", cmd.c_str(), nullptr);
    always_assert_log(ret != -1, "exec of command failed: %s", strerror(errno));
    not_reached();
  }
#else
  std::cerr << "spawn() is a no-op on non-POSIX systems" << std::endl;
  return 0;
#endif
}

/*
 * Appends the PID of the current process to :cmd and invokes it.
 */
pid_t spawn_profiler(const std::string& cmd) {
#ifdef _POSIX_VERSION
  auto parent = getpid();
  std::ostringstream ss;
  ss << cmd << " " << parent;
  auto full_cmd = ss.str();
  return spawn(full_cmd);
#else
  std::cerr << "spawn_profiler() is a no-op on non-POSIX systems" << std::endl;
  return 0;
#endif
}

pid_t kill_and_wait(pid_t pid, int sig) {
#ifdef _POSIX_VERSION
  kill(pid, sig);
  return waitpid(pid, nullptr, 0);
#else
  std::cerr << "kill_and_wait() is a no-op on non-POSIX systems" << std::endl;
  return 0;
#endif
}

void run_and_wait(const std::string& cmd) {
#ifdef _POSIX_VERSION
  auto child = spawn(cmd);
  if (child == 0) {
    return;
  }

  int status;
  pid_t wpid = waitpid(child, &status, 0);
  always_assert_log(wpid != -1, "Failed to waitpid: %s", strerror(errno));
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::cerr << "Failed cmd " << cmd << std::endl;
  }
#else
  std::cerr << "run_and_wait() is a no-op on non-POSIX systems" << std::endl;
#endif
}

} // namespace

ScopedCommandProfiling::ScopedCommandProfiling(
    const std::string& cmd,
    boost::optional<std::string> shutdown_cmd,
    boost::optional<std::string> post_cmd,
    const char* log_str) {
  m_shutdown_cmd = std::move(shutdown_cmd);
  m_post_cmd = std::move(post_cmd);
  if (log_str != nullptr) {
    std::cerr << "Running profiler " << log_str << "..." << std::endl;
  } else {
    std::cerr << "Running profiler..." << std::endl;
  }
  m_profiler = spawn_profiler(cmd);
}

ScopedCommandProfiling::~ScopedCommandProfiling() {
  if (m_profiler != -1) {
    std::cerr << "Waiting for profiler to finish..." << std::endl;
    if (m_shutdown_cmd) {
      run_and_wait(*m_shutdown_cmd);
    } else {
      kill_and_wait(m_profiler, SIGINT);
    }
    if (m_post_cmd) {
      run_and_wait(*m_post_cmd + " perf.data");
    }
  }
}

ScopedCommandProfiling::ScopedCommandProfiling(
    ScopedCommandProfiling&& other) noexcept {
  *this = std::move(other);
}

ScopedCommandProfiling& ScopedCommandProfiling::operator=(
    ScopedCommandProfiling&& rhs) noexcept {
  m_profiler = rhs.m_profiler;
  m_shutdown_cmd = std::move(rhs.m_shutdown_cmd);
  m_post_cmd = std::move(rhs.m_post_cmd);

  rhs.m_profiler = -1;

  return *this;
}

boost::optional<ScopedCommandProfiling::ProfilerInfo>
ScopedCommandProfiling::maybe_info_from_env(const std::string& prefix) {
  auto get_env_str =
      [](const std::string& key) -> boost::optional<std::string> {
    auto val = getenv(key.c_str());
    if (val == nullptr) {
      return boost::none;
    }
    return std::string(val);
  };

  std::string profiler_key = prefix + "PROFILE_COMMAND";
  if (getenv(profiler_key.c_str()) == nullptr) {
    return boost::none;
  }

  return boost::make_optional<ProfilerInfo>(
      ProfilerInfo{getenv(profiler_key.c_str()),
                   get_env_str(prefix + "PROFILE_SHUTDOWN_COMMAND"),
                   get_env_str(prefix + "PROFILE_POST_COMMAND")});
}

template <typename T>
boost::optional<ScopedCommandProfiling> ScopedCommandProfiling::maybe_from_info(
    const boost::optional<ProfilerInfo>& info, const T* log_str) {
  if (!info) {
    return boost::none;
  }
  return ScopedCommandProfiling(*info, log_str);
}
template boost::optional<ScopedCommandProfiling>
ScopedCommandProfiling::maybe_from_info<>(
    const boost::optional<ProfilerInfo>& info, const std::string* log_str);
template boost::optional<ScopedCommandProfiling>
ScopedCommandProfiling::maybe_from_info<>(
    const boost::optional<ProfilerInfo>& info, const char* log_str);
