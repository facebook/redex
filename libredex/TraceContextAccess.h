/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Macros.h"

#if !IS_WINDOWS

#include "Trace.h"

struct TraceContextAccess {
  static const TraceContext* get_s_context() { return TraceContext::s_context; }
};
#endif
