/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "VirtualRegistersFile.h"

#include "Debug.h"

namespace regalloc {

constexpr reg_t REG_MAX = std::numeric_limits<reg_t>::max();

reg_t VirtualRegistersFile::alloc(size_t width) {
  auto next_free = m_free.find_first();
  // find `width` consecutive bits in m_free that are set
outer:
  while (next_free != boost::dynamic_bitset<>::npos) {
    for (reg_t i = 1; i < width; ++i) {
      if (!m_free[next_free + i]) {
        next_free = m_free.find_next(next_free + i);
        goto outer;
      }
    }
    break;
  }
  if (next_free == boost::dynamic_bitset<>::npos) {
    next_free = find_free_range_at_end();
  }
  alloc_at(next_free, width);
  return next_free;
}

/*
 * Finds the last sequence of consecutive free registers that reaches the end
 * of the register file, and returns the first register of that range.
 */
reg_t VirtualRegistersFile::find_free_range_at_end() const {
  for (int i = m_free.size() - 1; i >= 0; --i) {
    if (!m_free[i]) {
      return i + 1;
    }
  }
  return 0;
}

void VirtualRegistersFile::alloc_at(reg_t pos, size_t width) {
  if (m_free.size() < pos + width) {
    m_free.resize(std::max(m_free.size(), pos + width), /* value */ 1);
    always_assert(m_free.size() <= REG_MAX);
  }
  for (size_t i = 0; i < width; ++i) {
    m_free[pos + i] = 0;
  }
}

void VirtualRegistersFile::free(reg_t n, size_t width) {
  for (size_t i = 0; i < width; ++i) {
    m_free.set(n + i);
  }
}

bool VirtualRegistersFile::is_free(reg_t pos, size_t width) const {
  for (size_t i = pos; i < std::min(m_free.size(), pos + width); ++i) {
    if (!m_free[i]) {
      return false;
    }
  }
  return true;
}

std::ostream& operator<<(std::ostream& o,
                         const VirtualRegistersFile& vreg_file) {
  for (reg_t i = 0; i < vreg_file.m_free.size(); ++i) {
    if (i > 0) {
      o << " ";
    }
    o << (!vreg_file.m_free[i] ? "!" : " ") << i;
  }
  return o;
}

} // namespace regalloc
