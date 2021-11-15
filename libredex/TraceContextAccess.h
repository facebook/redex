/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Trace.h"

#if !IS_WINDOWS
struct TraceContextAccess {
  static const TraceContext* get_s_context() { return TraceContext::s_context; }
};
#endif
