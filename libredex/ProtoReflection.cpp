/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "protores/ProtoReflection.h"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

namespace redex {
void reset_pb_source(google::protobuf::Message* message) {
  if (message == nullptr) {
    return;
  }
  const auto* descriptor = message->GetDescriptor();
  const auto* reflection = message->GetReflection();
  for (int i = 0; i < descriptor->field_count(); i++) {
    const auto* field = descriptor->field(i);
    const auto repeated = field->is_repeated();
    if (field->cpp_type() ==
            google::protobuf::FieldDescriptor::CPPTYPE_UINT32 &&
        reflection->HasField(*message, field) && !repeated) {
      const auto name = field->name();
      if (name == "path_idx" || name == "line_number" ||
          name == "column_number") {
        reflection->SetUInt32(message, field, 0);
      }
    } else if (field->cpp_type() ==
               google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      if (repeated) {
        for (int j = 0; j < reflection->FieldSize(*message, field); j++) {
          reset_pb_source(
              reflection->MutableRepeatedMessage(message, field, j));
        }
      } else if (reflection->HasField(*message, field)) {
        reset_pb_source(reflection->MutableMessage(message, field));
      }
    }
  }
}
} // namespace redex
