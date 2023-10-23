/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "RedexException.h"

#include <cstdint>
#include <iosfwd>
#include <stdexcept>

namespace redex_debug {

void set_exc_type_as_abort(RedexError type);
void disable_stack_trace_for_exc_type(RedexError type);

} // namespace redex_debug

void print_stack_trace(std::ostream& os, const std::exception& e);

void crash_backtrace_handler(int sig);
void debug_backtrace_handler(int sig);

// If `block` is true, only a single assert will be logged. All following
// asserts will sleep forever.
void block_multi_asserts(bool block);

// If called, assertions on threads other than the caller may immediately abort
// instead of raising an exception. Currently only implemented for Linux.
// Note: this is a workaround for libstdc++ from GCC < 8.
void set_abort_if_not_this_thread();

// Stats from /proc. See http://man7.org/linux/man-pages/man5/proc.5.html.
struct VmStats {
  uint64_t vm_peak = 0; // "Peak virtual memory size."
  uint64_t vm_hwm = 0; // "Peak resident set size ("high water mark")."
  uint64_t vm_rss = 0; // "Resident set size"
};
VmStats get_mem_stats();
bool try_reset_hwm_mem_stat(); // Attempt to reset the vm_hwm value.
