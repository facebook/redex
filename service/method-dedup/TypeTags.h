/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexUtil.h"

class TypeTags {

 public:
  void set_type_tag(const DexType* type, uint32_t type_tag);
  uint32_t get_type_tag(const DexType* type) const;
  const DexType* get_type(uint32_t type_tag) const;
  size_t size() const;

  typedef
      typename std::unordered_map<uint32_t, const DexType*>::iterator iterator;
  typedef typename std::unordered_map<uint32_t, const DexType*>::const_iterator
      const_iterator;

  iterator begin() { return m_tag_to_type.begin(); }
  const_iterator begin() const { return m_tag_to_type.begin(); }
  iterator end() { return m_tag_to_type.end(); }
  const_iterator end() const { return m_tag_to_type.end(); }
  bool has_type_tag(const DexType* type) const {
    return m_type_to_tag.find(type) != m_type_to_tag.end();
  }

 private:
  std::unordered_map<const DexType*, uint32_t> m_type_to_tag;
  std::unordered_map<uint32_t, const DexType*> m_tag_to_type;
};
