/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <boost/dynamic_bitset.hpp>

namespace regalloc {

using reg_t = uint16_t;

/*
 * This class tracks which registers are available over the course of register
 * allocation.
 *
 * Note that the naming may be kind of confusing: virtual registers are
 * "virtual" because they run on the Dalvik / ART virtual machine. However they
 * are subject to "physical" constraints like having wide data take up two
 * virtual registers. Registers that don't have these constraints -- e.g. the
 * instruction operands after live range numbering has been done -- are
 * referred to as "symbolic registers" or "symregs".
 */
class VirtualRegistersFile {
 public:
  /*
   * Finds the first empty slot of size :width in the register file and
   * allocates it. Returns the first register of that slot. Grow the register
   * file if necessary.
   */
  reg_t alloc(size_t width);

  /*
   * Allocates a slot of size :width at position :pos. Will not complain if the
   * slot is already allocated.
   */
  void alloc_at(reg_t pos, size_t width);

  /*
   * Frees a slot of size :width at :pos. Will not complain if the slot is
   * already free.
   */
  void free(reg_t pos, size_t width);

  /*
   * Returns whether :width registers are available at :pos.
   */
  bool is_free(reg_t pos, size_t width) const;

  reg_t size() const {
    return m_free.size();
  }

  friend std::ostream& operator<<(std::ostream&, const VirtualRegistersFile&);

 private:
  reg_t find_free_range_at_end() const;

  boost::dynamic_bitset<> m_free;
};

/*
 * Print out the file with exclamation marks indicating allocated slots. E.g.
 * "0 !1 2" means that we have a frame of size 3 and only register 1 is
 * allocated; the others are free.
 */
std::ostream& operator<<(std::ostream&, const VirtualRegistersFile&);

} // namespace regalloc
