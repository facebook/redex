/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

namespace google::protobuf {
class Message;
} // namespace google::protobuf

namespace redex {
void reset_pb_source(google::protobuf::Message* message);
} // namespace redex
