/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeTags.h"

void TypeTags::set_type_tag(const DexType* type, uint32_t type_tag) {
  m_type_to_tag.emplace(type, type_tag);
  m_tag_to_type.emplace(type_tag, type);
}

uint32_t TypeTags::get_type_tag(const DexType* type) const {
  return m_type_to_tag.at(type);
}

const DexType* TypeTags::get_type(uint32_t type_tag) const {
  return m_tag_to_type.at(type_tag);
}
