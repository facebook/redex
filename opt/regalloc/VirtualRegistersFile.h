/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/dynamic_bitset.hpp>

namespace regalloc {

using vreg_t = uint16_t;

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
  vreg_t alloc(size_t width);

  /*
   * Allocates a slot of size :width at position :pos. Will not complain if the
   * slot is already allocated.
   */
  void alloc_at(vreg_t pos, size_t width);

  /*
   * Frees a slot of size :width at :pos. Will not complain if the slot is
   * already free.
   */
  void free(vreg_t pos, size_t width);

  /*
   * Returns whether :width registers are available at :pos.
   */
  bool is_free(vreg_t pos, size_t width) const;

  vreg_t size() const { return m_free.size(); }

  friend std::ostream& operator<<(std::ostream&, const VirtualRegistersFile&);

 private:
  vreg_t find_free_range_at_end() const;

  boost::dynamic_bitset<> m_free;
};

/*
 * Print out the file with exclamation marks indicating allocated slots. E.g.
 * "0 !1 2" means that we have a frame of size 3 and only register 1 is
 * allocated; the others are free.
 */
std::ostream& operator<<(std::ostream&, const VirtualRegistersFile&);

} // namespace regalloc
