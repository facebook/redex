/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CommandProfiling.h"

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <sstream>

#include "Debug.h"

namespace {

/*
 * Appends the PID of the current process to :cmd and invokes it.
 */
pid_t spawn_profiler(const std::string& cmd) {
#ifdef _POSIX_VERSION
  auto parent = getpid();
  auto child = fork();
  if (child == -1) {
    always_assert_log(false, "Failed to fork");
    not_reached();
  } else if (child != 0) {
    return child;
  } else {
    std::ostringstream ss;
    ss << cmd << " " << parent;
    auto full_cmd = ss.str();
    execl("/bin/sh", "/bin/sh", "-c", full_cmd.c_str(), nullptr);
    always_assert_log(false, "exec of profiler command failed");
    not_reached();
  }
#else
  fprintf(stderr, "spawn_profiler() is a no-op on non-POSIX systems");
  return 0;
#endif
}

pid_t kill_and_wait(pid_t pid, int sig) {
#ifdef _POSIX_VERSION
  kill(pid, sig);
  return waitpid(pid, nullptr, 0);
#else
  fprintf(stderr, "kill_and_wait() is a no-op on non-POSIX systems");
  return 0;
#endif
}

} // namespace

ScopedCommandProfiling::ScopedCommandProfiling(
    boost::optional<std::string> cmd) {
  if (cmd) {
    fprintf(stderr, "Running profiler...\n");
    m_profiler = spawn_profiler(*cmd);
  }
}

ScopedCommandProfiling::~ScopedCommandProfiling() {
  if (m_profiler != -1) {
    fprintf(stderr, "Waiting for profiler to finish...\n");
    kill_and_wait(m_profiler, SIGINT);
  }
}
