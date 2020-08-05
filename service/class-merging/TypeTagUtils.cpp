/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeTagUtils.h"

#include <boost/optional.hpp>

#include "AnnoUtils.h"

namespace class_merging {

namespace type_tag_utils {

constexpr const char* MODEL_IDENTITY =
    "Lcom/facebook/redex/annotations/ModelIdentity;";

boost::optional<uint32_t> parse_model_type_tag(const DexClass* model_cls) {
  auto model_anno = DexType::get_type(DexString::get_string(MODEL_IDENTITY));
  always_assert_log(model_anno != nullptr, "Annotation %s not found!",
                    MODEL_IDENTITY);
  std::unordered_set<DexType*> anno_types = {model_anno};
  if (!has_any_annotation(model_cls, anno_types)) {
    return boost::none;
  }
  return parse_int_anno_value(model_cls, model_anno, "typeTag");
}

} // namespace type_tag_utils

} // namespace class_merging
